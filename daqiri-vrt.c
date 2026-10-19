// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * framing=vrt streaming for the DAQIRI backend: one RX poller thread per
 * open buffer drains one engine (port, queue), runs every datagram through
 * the VRT sequence tracker and assembles the payloads into the blocks the
 * application queued, in order.
 *
 * Block states:
 *   FREE -enqueue-> QUEUED -poller-> FILLING -full-> DONE -dequeue-> FREE
 *   a gap with lossy=0, or a stream failure: QUEUED/FILLING -> ERROR(err)
 *   cancel: QUEUED/FILLING -> CANCELLED
 * Dequeue returns 0 for DONE, err for ERROR and -ECANCELED for CANCELLED,
 * all three back to FREE; -EPERM for a FREE block. A pending block gives
 * -EBUSY without waiting, or -ETIMEDOUT after the timeout, still queued.
 *
 * A packet that arrives with no block queued is dropped (dropped_no_block,
 * the host overrun path), and the assembler forgets the carry and pending
 * zeros it had for the stream: the next block starts at the next packet.
 * -EPROTO from the tracker (layout mismatch) fails the stream: the current
 * and queued blocks, and every block enqueued afterwards, fail with -EPROTO
 * until the buffer is disabled.
 *
 * Locking: v->lock covers the block states, the FIFO, the tracker, the
 * assembler and the counters. The poller holds it while it processes a
 * burst, never while it polls the engine or sleeps.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* ppoll() */
#endif

#include <errno.h>
#include <iio/iio.h>
#include <inttypes.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "daqiri-private.h"
#include "daqiri-vrt-stream.h"

/* Empty polls spun through after traffic before the poller sleeps */
#define DQ_VRT_SPIN 64

/* Lossy zero-fill covers at most this many lost packets; a larger sized gap
 * is a resync instead: the current block (or, between blocks, the next one)
 * fails with -EIO, no zeros. */
#define DQ_VRT_MAX_FILL_PACKETS 1024

/* Bytes the assembler can carry over to the next block: one UDP payload */
#define DQ_VRT_CARRY_CAP 65536

#define DQ_VRT_PKTS_INIT 64

/* Stale bursts dropped at enable, at most (the sender may never pause) */
#define DQ_VRT_DRAIN_MAX 1024

struct dq_vrt_block {
	struct dq_vrt_block *next;
	void *data;
	size_t size;
	enum {
		DQ_BLK_FREE,
		DQ_BLK_QUEUED,
		DQ_BLK_FILLING,
		DQ_BLK_DONE,
		DQ_BLK_ERROR,
		DQ_BLK_CANCELLED,
	} state;
	int err;
};

struct dq_vrt_buffer {
	struct dq_rxq *q;
	enum dq_mem mem;
	int gpu;
	const char *engine_kind;
	struct dq_vrt_cfg cfg;
	struct timespec poll_ts; /* idle wait: poll_us; zero never sleeps */

	pthread_mutex_t lock;
	pthread_cond_t cond; /* a block left QUEUED/FILLING */
	int wake_fd;	     /* eventfd: wakes the sleeping poller */
	pthread_t thread;
	bool running;	  /* the thread exists; only enable/close touch it */
	atomic_bool stop; /* tells the poller to return */

	bool cancelled;
	int fatal; /* stream failure; every block fails with it */

	struct dq_vrt_block *head, *tail; /* QUEUED, in order */
	struct dq_vrt_block *cur;	  /* FILLING */
	bool cur_gap;			  /* cur follows an unsized loss */

	struct dq_vrt_rx rx;
	struct dq_asm as;

	uint64_t dropped_no_block;
	uint64_t blocks_completed, blocks_failed, blocks_with_gaps;
	int last_error;

	/* Poller only */
	struct dq_pkt *pkts;
	unsigned int pkts_cap;
};

enum dq_vrt_attr {
	DQ_ATTR_RX_PACKETS,
	DQ_ATTR_RX_BYTES,
	DQ_ATTR_RX_LOST_PACKETS,
	DQ_ATTR_RX_GAP_EVENTS,
	DQ_ATTR_RX_DROPPED_NO_BLOCK,
	DQ_ATTR_RX_FOREIGN_PACKETS,
	DQ_ATTR_RX_CONTEXT_PACKETS,
	DQ_ATTR_RX_MALFORMED,
	DQ_ATTR_RX_ZERO_FILLED_BYTES,
	DQ_ATTR_RX_LATE_PACKETS,
	DQ_ATTR_RX_DUPLICATE_PACKETS,
	DQ_ATTR_BLOCKS_COMPLETED,
	DQ_ATTR_BLOCKS_FAILED,
	DQ_ATTR_BLOCKS_WITH_GAPS,
	DQ_ATTR_STREAM_ID,
	DQ_ATTR_LAST_ERROR,
	DQ_ATTR_LOSSY,
	DQ_ATTR_MEM_DOMAIN,
	DQ_ATTR_ENGINE_KIND,
	DQ_ATTR_COUNT,
};

const char *const dq_vrt_attr_names[] = {
	[DQ_ATTR_RX_PACKETS] = "daqiri_rx_packets",
	[DQ_ATTR_RX_BYTES] = "daqiri_rx_bytes",
	[DQ_ATTR_RX_LOST_PACKETS] = "daqiri_rx_lost_packets",
	[DQ_ATTR_RX_GAP_EVENTS] = "daqiri_rx_gap_events",
	[DQ_ATTR_RX_DROPPED_NO_BLOCK] = "daqiri_rx_dropped_no_block",
	[DQ_ATTR_RX_FOREIGN_PACKETS] = "daqiri_rx_foreign_packets",
	[DQ_ATTR_RX_CONTEXT_PACKETS] = "daqiri_rx_context_packets",
	[DQ_ATTR_RX_MALFORMED] = "daqiri_rx_malformed",
	[DQ_ATTR_RX_ZERO_FILLED_BYTES] = "daqiri_rx_zero_filled_bytes",
	[DQ_ATTR_RX_LATE_PACKETS] = "daqiri_rx_late_packets",
	[DQ_ATTR_RX_DUPLICATE_PACKETS] = "daqiri_rx_duplicate_packets",
	[DQ_ATTR_BLOCKS_COMPLETED] = "daqiri_blocks_completed",
	[DQ_ATTR_BLOCKS_FAILED] = "daqiri_blocks_failed",
	[DQ_ATTR_BLOCKS_WITH_GAPS] = "daqiri_blocks_with_gaps",
	[DQ_ATTR_STREAM_ID] = "daqiri_stream_id",
	[DQ_ATTR_LAST_ERROR] = "daqiri_last_error",
	[DQ_ATTR_LOSSY] = "daqiri_lossy",
	[DQ_ATTR_MEM_DOMAIN] = "daqiri_mem_domain",
	[DQ_ATTR_ENGINE_KIND] = "daqiri_engine_kind",
};

const unsigned int dq_vrt_nb_attrs = DQ_ATTR_COUNT;

static void dq_vrt_wake(struct dq_vrt_buffer *v)
{
	uint64_t one = 1;

	if (write(v->wake_fd, &one, sizeof(one)) < 0) {
		/* Only fails when the counter is saturated: already awake */
	}
}

/* Idle: wait for the wake fd at most poll_us (microsecond resolution) */
static void dq_vrt_sleep(struct dq_vrt_buffer *v)
{
	struct pollfd pfd = { .fd = v->wake_fd, .events = POLLIN };
	uint64_t val;

	if (ppoll(&pfd, 1, &v->poll_ts, NULL) > 0 && (pfd.revents & POLLIN)) {
		if (read(v->wake_fd, &val, sizeof(val)) < 0) {
			/* Nothing to drain */
		}
	}
}

/* Finish the current block: DONE, or ERROR(err) */
static void dq_vrt_complete(struct dq_vrt_buffer *v, int err)
{
	struct dq_vrt_block *b = v->cur;

	if (v->cur_gap || v->as.had_gap)
		v->blocks_with_gaps++;

	if (err) {
		b->state = DQ_BLK_ERROR;
		b->err = err;
		v->blocks_failed++;
		v->last_error = err;
	} else {
		b->state = DQ_BLK_DONE;
		v->blocks_completed++;
	}

	v->cur = NULL;
	pthread_cond_broadcast(&v->cond);
}

/* Make the next queued block current. Blocks that the carry or pending
 * zeros fill on their own complete here. False when none is queued. */
static bool dq_vrt_next(struct dq_vrt_buffer *v)
{
	struct dq_vrt_block *b;

	while ((b = v->head)) {
		v->head = b->next;
		if (!v->head)
			v->tail = NULL;
		b->next = NULL;

		b->state = DQ_BLK_FILLING;
		v->cur = b;
		v->cur_gap = false;
		dq_asm_attach(&v->as, b->data, b->size);
		if (!dq_asm_done(&v->as))
			return true;

		dq_vrt_complete(v, 0);
	}

	return false;
}

/* Fail the current and the queued blocks, and those enqueued later */
static void dq_vrt_fail_stream(struct dq_vrt_buffer *v, int err)
{
	struct dq_vrt_block *b;

	v->fatal = err;
	if (v->cur)
		dq_vrt_complete(v, err);

	while ((b = v->head)) {
		v->head = b->next;
		b->next = NULL;
		b->state = DQ_BLK_ERROR;
		b->err = err;
		v->blocks_failed++;
	}
	v->tail = NULL;
	v->last_error = err;

	dq_asm_reset(&v->as);
	pthread_cond_broadcast(&v->cond);
}

/* Assemble one payload that follows <gap> lost packets */
static void dq_vrt_deliver(struct dq_vrt_buffer *v, const uint8_t *payload, size_t len,
		uint32_t gap, bool flag)
{
	bool done;
	int ret;

	for (;;) {
		if (!v->cur && !dq_vrt_next(v)) {
			v->dropped_no_block++;
			/* The next block starts with the next packet */
			dq_asm_reset(&v->as);
			return;
		}

		if (flag)
			v->cur_gap = true;

		ret = dq_asm_push(&v->as, payload, len, gap, &done);
		if (ret >= 0) {
			if (done)
				dq_vrt_complete(v, 0);
			return;
		}

		if (ret == -ENOSPC) {
			/* Not expected: a full block never stays current */
			dq_vrt_complete(v, 0);
			continue;
		}

		dq_vrt_complete(v, ret);
		if (ret != -EIO) {
			/* -ENOBUFS: a payload larger than block plus carry */
			dq_asm_reset(&v->as);
			return;
		}

		/* Gap with lossy=0: this packet starts the next block */
		gap = 0;
	}
}

static void dq_vrt_packet(struct dq_vrt_buffer *v, const uint8_t *data, size_t len)
{
	struct dq_vrt_data out;
	bool flag = false;
	uint32_t gap;
	int ret;

	ret = dq_vrt_rx_packet(&v->rx, data, len, &out);
	if (ret == DQ_VRT_SKIP)
		return;
	if (ret < 0) {
		if (!v->fatal)
			dq_vrt_fail_stream(v, ret);
		return;
	}
	if (v->fatal)
		return;

	gap = out.gap;
	if (out.gap_unknown) {
		/* A break of unknown size: fail like a sized gap, or with
		 * lossy=1 flag the block without zero-filling */
		if (v->as.lossy) {
			gap = 0;
			flag = true;
		} else if (!gap) {
			gap = 1;
		}
	} else if (v->as.lossy && gap > DQ_VRT_MAX_FILL_PACKETS) {
		/* Too large to fill: resync on this packet. A block fails as
		 * with lossy=0: the current one, or with none current (the gap
		 * fell on a block boundary) the next one, attached empty after
		 * dropping what the assembler owed. */
		if (!v->cur) {
			dq_asm_reset(&v->as);
			dq_vrt_next(v);
		}
		if (v->cur) {
			v->cur_gap = true;
			dq_vrt_complete(v, -EIO);
		}
		dq_asm_reset(&v->as);
		gap = 0;
	}

	dq_vrt_deliver(v, out.payload, out.len, gap, flag);
}

static void dq_vrt_burst(struct dq_vrt_buffer *v, const struct dq_burst *b)
{
	struct dq_pkt *pkts;
	unsigned int i, n;

	if (b->nb_pkts > v->pkts_cap) {
		pkts = realloc(v->pkts, b->nb_pkts * sizeof(*pkts));
		if (!pkts) {
			pthread_mutex_lock(&v->lock);
			if (!v->fatal)
				dq_vrt_fail_stream(v, -ENOMEM);
			pthread_mutex_unlock(&v->lock);
			return;
		}
		v->pkts = pkts;
		v->pkts_cap = b->nb_pkts;
	}

	n = dq_burst_pkts(b, v->pkts, v->pkts_cap);

	pthread_mutex_lock(&v->lock);
	for (i = 0; i < n; i++) {
		if (v->pkts[i].len < b->l2_offset) {
			v->rx.c.malformed++;
			continue;
		}
		dq_vrt_packet(v, v->pkts[i].data + b->l2_offset, v->pkts[i].len - b->l2_offset);
	}
	pthread_mutex_unlock(&v->lock);
}

static void *dq_vrt_poller(void *d)
{
	struct dq_vrt_buffer *v = d;
	unsigned int idle = 0;
	struct dq_burst b;
	int ret;

	/* The default 50 us timer slack would double a 50 us idle wait */
	prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);

	while (!atomic_load(&v->stop)) {
		ret = dq_rxq_poll(v->q, &b);
		if (ret == -EAGAIN) {
			/* Spin a little after traffic, then sleep; idle stays
			 * at the limit until the next burst */
			if (idle < DQ_VRT_SPIN)
				idle++;
			else
				dq_vrt_sleep(v);
			continue;
		}

		idle = 0;
		if (ret < 0) {
			pthread_mutex_lock(&v->lock);
			if (!v->fatal)
				dq_vrt_fail_stream(v, ret);
			pthread_mutex_unlock(&v->lock);
			dq_vrt_sleep(v);
			continue;
		}

		dq_vrt_burst(v, &b);
		dq_burst_free(&b);
	}

	return NULL;
}

struct dq_vrt_buffer *dq_vrt_open(struct iio_buffer_pdata *buf, struct dq_rxq *q,
		const struct dq_opts *o, enum dq_swap swap, size_t sample_size)
{
	struct dq_vrt_buffer *v;
	pthread_condattr_t attr;
	int ret;

	v = calloc(1, sizeof(*v));
	if (!v)
		return iio_ptr(-ENOMEM);

	v->q = q;
	v->mem = o->mem;
	v->gpu = o->gpu;
	v->engine_kind = buf->ctx->engine_kind;
	v->poll_ts.tv_sec = (time_t)(o->poll_us / 1000000);
	v->poll_ts.tv_nsec = (long)(o->poll_us % 1000000) * 1000L;
	v->cfg.sid = o->sid;
	v->cfg.sid_mask = o->sid_mask;
	v->cfg.gap_ts = o->gap_ts;
	v->cfg.sample_size = sample_size;
	atomic_init(&v->stop, true);
	dq_vrt_rx_init(&v->rx, &v->cfg);

	ret = dq_asm_init(&v->as, swap, o->lossy, DQ_VRT_CARRY_CAP);
	if (ret)
		goto err_free;

	v->pkts = calloc(DQ_VRT_PKTS_INIT, sizeof(*v->pkts));
	if (!v->pkts) {
		ret = -ENOMEM;
		goto err_asm;
	}
	v->pkts_cap = DQ_VRT_PKTS_INIT;

	v->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (v->wake_fd < 0) {
		ret = -errno;
		goto err_pkts;
	}

	ret = -pthread_mutex_init(&v->lock, NULL);
	if (ret)
		goto err_close;

	/* Dequeue timeouts are durations: wait on the monotonic clock */
	ret = -pthread_condattr_init(&attr);
	if (!ret) {
		ret = -pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
		if (!ret)
			ret = -pthread_cond_init(&v->cond, &attr);
		pthread_condattr_destroy(&attr);
	}
	if (ret)
		goto err_mutex;

	return v;

err_mutex:
	pthread_mutex_destroy(&v->lock);
err_close:
	close(v->wake_fd);
err_pkts:
	free(v->pkts);
err_asm:
	dq_asm_fini(&v->as);
err_free:
	free(v);
	return iio_ptr(ret);
}

/* Stop and join the poller */
static void dq_vrt_stop(struct dq_vrt_buffer *v)
{
	atomic_store(&v->stop, true);
	dq_vrt_wake(v);

	if (v->running) {
		pthread_join(v->thread, NULL);
		v->running = false;
	}
}

int dq_vrt_enable(struct dq_vrt_buffer *v, bool enable)
{
	struct dq_vrt_counters c;
	struct dq_burst b;
	unsigned int i;
	int ret;

	if (!enable) {
		dq_vrt_stop(v);

		pthread_mutex_lock(&v->lock);
		/* A partly filled block starts over from the next packet */
		if (v->cur) {
			v->cur->state = DQ_BLK_QUEUED;
			v->cur->next = v->head;
			v->head = v->cur;
			if (!v->tail)
				v->tail = v->cur;
			v->cur = NULL;
		}
		v->fatal = 0;
		pthread_mutex_unlock(&v->lock);
		return 0;
	}

	pthread_mutex_lock(&v->lock);
	if (v->cancelled || v->running) {
		pthread_mutex_unlock(&v->lock);
		return v->cancelled ? -ECANCELED : 0;
	}

	/* A new stream: the tracker resyncs (counters kept) and the
	 * assembler forgets the old position */
	c = v->rx.c;
	dq_vrt_rx_init(&v->rx, &v->cfg);
	v->rx.c = c;
	dq_asm_reset(&v->as);
	pthread_mutex_unlock(&v->lock);

	/* Whatever queued up while disabled is stale */
	for (i = 0; i < DQ_VRT_DRAIN_MAX && !dq_rxq_poll(v->q, &b); i++)
		dq_burst_free(&b);

	atomic_store(&v->stop, false);
	ret = -pthread_create(&v->thread, NULL, dq_vrt_poller, v);
	if (ret) {
		atomic_store(&v->stop, true);
		return ret;
	}

	v->running = true;
	return 0;
}

void dq_vrt_cancel(struct dq_vrt_buffer *v)
{
	struct dq_vrt_block *b;

	pthread_mutex_lock(&v->lock);
	v->cancelled = true;

	if (v->cur) {
		v->cur->state = DQ_BLK_CANCELLED;
		v->cur = NULL;
	}
	while ((b = v->head)) {
		v->head = b->next;
		b->next = NULL;
		b->state = DQ_BLK_CANCELLED;
	}
	v->tail = NULL;
	dq_asm_reset(&v->as);

	pthread_cond_broadcast(&v->cond);
	pthread_mutex_unlock(&v->lock);

	atomic_store(&v->stop, true);
	dq_vrt_wake(v);
}

void dq_vrt_close(struct dq_vrt_buffer *v)
{
	dq_vrt_cancel(v);
	dq_vrt_stop(v);

	pthread_cond_destroy(&v->cond);
	pthread_mutex_destroy(&v->lock);
	close(v->wake_fd);
	free(v->pkts);
	dq_asm_fini(&v->as);
	free(v);
}

struct dq_vrt_block *dq_vrt_block_new(struct dq_vrt_buffer *v, size_t size, void **data)
{
	struct dq_vrt_block *b;
	int ret;

	/* Blocks hold whole samples: the assembler never splits one */
	if (!size || size % v->cfg.sample_size)
		return iio_ptr(-EINVAL);

	b = calloc(1, sizeof(*b));
	if (!b)
		return iio_ptr(-ENOMEM);

	ret = dq_mem_alloc(v->mem, v->gpu, size, &b->data);
	if (ret) {
		free(b);
		return iio_ptr(ret);
	}

	b->size = size;
	b->state = DQ_BLK_FREE;
	*data = b->data;

	return b;
}

void dq_vrt_block_free(struct dq_vrt_buffer *v, struct dq_vrt_block *b)
{
	struct dq_vrt_block **p;

	pthread_mutex_lock(&v->lock);
	if (b == v->cur) {
		/* The poller must not write into it any more */
		v->cur = NULL;
		dq_asm_reset(&v->as);
	} else if (b->state == DQ_BLK_QUEUED) {
		for (p = &v->head; *p; p = &(*p)->next) {
			if (*p == b) {
				*p = b->next;
				break;
			}
		}
		for (v->tail = v->head; v->tail && v->tail->next; v->tail = v->tail->next)
			;
	}
	pthread_mutex_unlock(&v->lock);

	dq_mem_free(v->mem, b->data, b->size);
	free(b);
}

int dq_vrt_enqueue(struct dq_vrt_buffer *v, struct dq_vrt_block *b, size_t bytes_used)
{
	int ret = 0;

	pthread_mutex_lock(&v->lock);

	if (b->state != DQ_BLK_FREE) {
		ret = -EPERM;
	} else if (bytes_used != b->size) {
		/* RX blocks are always filled completely */
		ret = -EINVAL;
	} else if (v->cancelled) {
		ret = -ECANCELED;
	} else if (v->fatal) {
		b->state = DQ_BLK_ERROR;
		b->err = v->fatal;
		v->blocks_failed++;
	} else {
		b->state = DQ_BLK_QUEUED;
		b->next = NULL;
		if (v->tail)
			v->tail->next = b;
		else
			v->head = b;
		v->tail = b;
	}

	pthread_mutex_unlock(&v->lock);

	return ret;
}

static void dq_vrt_deadline(struct timespec *ts, int timeout_ms)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ts->tv_sec += timeout_ms / 1000;
	ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* timeout_ms is the resolved context timeout: < 0 waits forever, 0 does
 * not wait (IIO_TIMEOUT_NONBLOCK) */
int dq_vrt_dequeue(struct dq_vrt_buffer *v, struct dq_vrt_block *b, bool nonblock, int timeout_ms)
{
	bool timed_out = false;
	struct timespec ts;
	int ret;

	if (timeout_ms > 0)
		dq_vrt_deadline(&ts, timeout_ms);

	pthread_mutex_lock(&v->lock);

	for (;;) {
		switch (b->state) {
		case DQ_BLK_FREE:
			ret = -EPERM;
			goto out_unlock;
		case DQ_BLK_DONE:
			ret = 0;
			b->state = DQ_BLK_FREE;
			goto out_unlock;
		case DQ_BLK_ERROR:
			ret = b->err;
			b->state = DQ_BLK_FREE;
			goto out_unlock;
		case DQ_BLK_CANCELLED:
			ret = -ECANCELED;
			b->state = DQ_BLK_FREE;
			goto out_unlock;
		default:
			break;
		}

		if (nonblock) {
			ret = -EBUSY;
			goto out_unlock;
		}
		if (timed_out || !timeout_ms) {
			ret = -ETIMEDOUT;
			goto out_unlock;
		}

		if (timeout_ms < 0)
			pthread_cond_wait(&v->cond, &v->lock);
		else
			timed_out = pthread_cond_timedwait(&v->cond, &v->lock, &ts) == ETIMEDOUT;
	}

out_unlock:
	pthread_mutex_unlock(&v->lock);

	return ret;
}

static int dq_vrt_attr_index(const char *name)
{
	unsigned int i;

	for (i = 0; i < dq_vrt_nb_attrs; i++)
		if (!strcmp(name, dq_vrt_attr_names[i]))
			return (int)i;

	return -ENOENT;
}

ssize_t dq_vrt_read_attr(struct dq_vrt_buffer *v, const char *name, char *dst, size_t len)
{
	struct dq_vrt_counters c;
	uint64_t u = 0, zero_filled, dropped, completed, failed, with_gaps;
	int idx = dq_vrt_attr_index(name), last_error, lossy, n;
	uint32_t sid;

	if (idx < 0)
		return idx;

	pthread_mutex_lock(&v->lock);
	c = v->rx.c;
	zero_filled = v->as.zero_filled;
	dropped = v->dropped_no_block;
	completed = v->blocks_completed;
	failed = v->blocks_failed;
	with_gaps = v->blocks_with_gaps;
	last_error = v->last_error;
	lossy = v->as.lossy;
	sid = v->cfg.sid_mask ? v->cfg.sid : v->rx.sid_locked ? v->rx.sid : 0;
	pthread_mutex_unlock(&v->lock);

	switch (idx) {
	case DQ_ATTR_RX_PACKETS:
		u = c.packets;
		break;
	case DQ_ATTR_RX_BYTES:
		u = c.bytes;
		break;
	case DQ_ATTR_RX_LOST_PACKETS:
		u = c.lost_packets;
		break;
	case DQ_ATTR_RX_GAP_EVENTS:
		u = c.gap_events;
		break;
	case DQ_ATTR_RX_DROPPED_NO_BLOCK:
		u = dropped;
		break;
	case DQ_ATTR_RX_FOREIGN_PACKETS:
		u = c.foreign;
		break;
	case DQ_ATTR_RX_CONTEXT_PACKETS:
		u = c.context;
		break;
	case DQ_ATTR_RX_MALFORMED:
		u = c.malformed;
		break;
	case DQ_ATTR_RX_ZERO_FILLED_BYTES:
		u = zero_filled;
		break;
	case DQ_ATTR_RX_LATE_PACKETS:
		u = c.late;
		break;
	case DQ_ATTR_RX_DUPLICATE_PACKETS:
		u = c.duplicates;
		break;
	case DQ_ATTR_BLOCKS_COMPLETED:
		u = completed;
		break;
	case DQ_ATTR_BLOCKS_FAILED:
		u = failed;
		break;
	case DQ_ATTR_BLOCKS_WITH_GAPS:
		u = with_gaps;
		break;
	case DQ_ATTR_STREAM_ID:
		n = sid ? snprintf(dst, len, "0x%08" PRIx32, sid) : snprintf(dst, len, "0");
		goto out;
	case DQ_ATTR_LAST_ERROR:
		n = snprintf(dst, len, "%d", last_error);
		goto out;
	case DQ_ATTR_LOSSY:
		n = snprintf(dst, len, "%d", lossy);
		goto out;
	case DQ_ATTR_MEM_DOMAIN:
		n = snprintf(dst, len, "%s", dq_mem_name(v->mem));
		goto out;
	default: /* DQ_ATTR_ENGINE_KIND */
		n = snprintf(dst, len, "%s", v->engine_kind);
		goto out;
	}

	n = snprintf(dst, len, "%" PRIu64, u);
out:
	if (n < 0)
		return -EIO;
	if ((size_t)n >= len)
		return -ENOSPC;

	return (ssize_t)n + 1;
}

int dq_vrt_write_attr(struct dq_vrt_buffer *v, const char *name, const char *src, size_t len)
{
	bool lossy;

	if (strcmp(name, dq_vrt_attr_names[DQ_ATTR_LOSSY]))
		return dq_vrt_attr_index(name) < 0 ? -ENOENT : -EPERM;

	/* "0" or "1", optionally followed by a newline and/or a NUL */
	if (!len || (src[0] != '0' && src[0] != '1') ||
			(len > 1 && src[1] != '\0' && src[1] != '\n') ||
			(len > 2 && src[1] == '\n' && src[2] != '\0'))
		return -EINVAL;
	lossy = src[0] == '1';

	pthread_mutex_lock(&v->lock);
	v->as.lossy = lossy;
	pthread_mutex_unlock(&v->lock);

	return 0;
}
