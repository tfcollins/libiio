/* SPDX-License-Identifier: MIT */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * UDP sample mode end to end: daqiri:127.0.0.1,data=udp over the DAQIRI
 * socket engine (DAQIRI_YAML: socket-udp-loopback.yaml, which receives on
 * 127.0.0.1:25101 from 127.0.0.1:25102 only) against iiod-emu --udp, the
 * reference server, which sends from its default 127.0.0.1:25102.
 *
 * The emulator serves fmcomms2.xml. For cf-ad9361-lpc voltage0+voltage1
 * (4-byte samples) it answers the backend's 9200-byte limit with 9200-byte
 * payloads, cut from cf-ad9361-lpc_buf0.bin as an endless byte stream:
 * datagram s carries the file's bytes from (s * 9200) mod <file length> on,
 * wrapping at the end of the file, within a datagram too. A block of 9200
 * bytes is found to hold the payload of datagram s by comparing every byte
 * with the file, or to be all zero.
 *
 * Every case runs against an emulator of its own, which with_emu.sh starts
 * with the case's injection flags and --udp-json <oracle.json>. Without
 * --case this program is the driver that runs every case that way:
 *   test_udp_loopback <with_emu.sh> <xml> <sample.bin> <oracle.json>
 *   test_udp_loopback --case <name> <sample.bin> <oracle.json>
 *
 * A case queues 64 one-datagram blocks, enables, dequeues blocks in order and
 * queues each one again, then disables; the blocks that completed by then are
 * dequeued afterwards. At the disable the emulator writes its oracle: the
 * datagrams it sent (copies included), dropped, reordered (pairs, counted at
 * the late datagram) and duplicated (copies). The blocks must be what the
 * emulator's order of emission gives under the host's rules, and the counters
 * must match the oracle:
 * - every datagram sent arrived: rx_packets + late + duplicates == sent;
 * - late == reordered, duplicates == duplicated;
 * - lost == dropped less the drops past the last datagram sent, which the
 *   host cannot see: the sequence numbers 0 .. last are the ones sent once
 *   (sent - duplicated) and the dropped ones, the trailing drops the rest;
 * - a gap fails one block with lossy=0 and zero-fills one block per lost
 *   datagram with lossy=1, exactly.
 * The emulator is paced (--udp-pps) in every case: unpaced on loopback, a
 * stop that meets a full socket can leave a batch partly unsent.
 */

#include <errno.h>
#include <iio/iio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_framework.h"

#define DEV_NAME "cf-ad9361-lpc"

/* voltage0+voltage1: the YAML's 9216-byte buffers less the 16-byte header,
 * a multiple of 16 and of the 4-byte sample set */
#define PAYLOAD 9200
#define NB_BLOCKS 64
/* Blocks dequeued before the disable */
#define RUN_BLOCKS 64

#define MAX_RECS 512
/* Sequence numbers told apart by their payload: (s * PAYLOAD) mod the file
 * length must differ for all of them */
#define MAX_SEQ 1024

#define DEQUEUE_TIMEOUT_MS 3000
#define WAIT_TIMEOUT_MS 3000

/* A block, as dequeued or as expected: the datagram it holds (>= 0) or */
#define BLK_EIO (-1)  /* dequeue returned -EIO */
#define BLK_ZERO (-2) /* all zero */
#define BLK_BAD (-3)  /* neither all zero nor the payload of one datagram */
#define BLK_ERR (-4)  /* another error; ends the case */

#define MAX_STREAMS 8

/* CHECK_EQ() evaluates <actual> again to print it: evaluate once */
#define CHECK_EQ(actual, expected, message) \
	do { \
		long check_val_ = (long)(actual); \
		TEST_ASSERT_EQ(check_val_, (long)(expected), message); \
	} while (0)

struct oracle {
	unsigned long long sent, dropped, reordered, duplicated;
};

/* What the emulator injects, for the model of its order of emission */
struct inject {
	const long *drop_list;
	unsigned int nb_drop;
	long reorder_window;
	long dup_every;
};

/* The daqiri_* counters of a buffer */
struct counters {
	long long packets, bytes, lost, gaps, dropped_no_block, overruns, foreign, malformed,
			zero_filled, late, duplicates, completed, failed, with_gaps, last_error;
};

/* The streams in the emulator's log: every setup, every start */
struct streams {
	uint32_t setup[MAX_STREAMS], started[MAX_STREAMS];
	unsigned int payload[MAX_STREAMS];
	unsigned int nb_setup, nb_started;
};

struct lb {
	struct iio_context *ctx;
	struct iio_buffer *buf;
	struct iio_channels_mask *mask;
	struct iio_buffer_stream *stream;
	struct iio_block *blocks[NB_BLOCKS];
	unsigned int nb_blocks;
	unsigned int next; /* the block to dequeue next: the queue is in order */
	long recs[MAX_RECS];
	unsigned int nb_recs;
};

static const char *yaml, *oracle_path;
static uint8_t *file;
static size_t file_len;
static long *seq_at; /* by offset in the file: the datagram whose payload starts there */

/* One at a time; too large for the stack */
static struct lb lb;

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static long ms_since(const struct timespec *t0)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (long)(now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

static int read_file(const char *path, uint8_t **data, size_t *len)
{
	uint8_t *buf = NULL, *p;
	size_t n, size = 0;
	FILE *f;
	int ret = 0;

	f = fopen(path, "rb");
	if (!f)
		return -errno;

	for (;;) {
		p = realloc(buf, size + 65536);
		if (!p) {
			ret = -ENOMEM;
			break;
		}
		buf = p;
		n = fread(buf + size, 1, 65536, f);
		size += n;
		if (n < 65536) {
			if (ferror(f))
				ret = -EIO;
			break;
		}
	}
	fclose(f);

	if (ret) {
		free(buf);
		return ret;
	}

	*data = buf;
	*len = size;

	return 0;
}

/* Which datagram's payload starts at which offset of the file; -EINVAL when
 * two of the first MAX_SEQ start at the same one */
static int map_offsets(void)
{
	size_t off;
	long s;

	seq_at = malloc(file_len * sizeof(*seq_at));
	if (!seq_at)
		return -ENOMEM;

	for (off = 0; off < file_len; off++)
		seq_at[off] = -1;

	for (s = 0; s < MAX_SEQ; s++) {
		off = (size_t)((uint64_t)s * PAYLOAD % file_len);
		if (seq_at[off] >= 0)
			return -EINVAL;
		seq_at[off] = s;
	}

	return 0;
}

static bool stream_at(const uint8_t *p, size_t off)
{
	size_t i;

	for (i = 0; i < PAYLOAD; i++) {
		if (p[i] != file[(off + i) % file_len])
			return false;
	}

	return true;
}

/* BLK_ZERO, the datagram whose payload the block is, or BLK_BAD */
static long classify(const struct iio_block *block)
{
	const uint8_t *p = iio_block_start(block);
	size_t off, i;

	for (i = 0; i < PAYLOAD && !p[i]; i++)
		;
	if (i == PAYLOAD)
		return BLK_ZERO;

	/* The file has no period shorter than itself and a payload is longer:
	 * at most one offset matches */
	for (off = 0; off < file_len; off++) {
		if (p[0] == file[off] && stream_at(p, off))
			return seq_at[off] >= 0 ? seq_at[off] : BLK_BAD;
	}

	return BLK_BAD;
}

static const char *blk_name(long v, char *buf, size_t len)
{
	static const char *const names[] = { "-EIO", "zero", "bad", "error" };

	if (v >= 0)
		snprintf(buf, len, "datagram %ld", v);
	else
		snprintf(buf, len, "%s", names[-v - 1]);

	return buf;
}

static bool json_number(const char *json, const char *key, unsigned long long *val)
{
	char name[32], *end;
	const char *p;

	snprintf(name, sizeof(name), "\"%s\"", key);
	p = strstr(json, name);
	if (!p)
		return false;

	p += strlen(name);
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	if (*p++ != ':')
		return false;

	errno = 0;
	*val = strtoull(p, &end, 10);

	return end != p && !errno;
}

/* The emulator replaces the file in one go at every disable */
static int oracle_read(struct oracle *o)
{
	uint8_t *data;
	size_t len;
	char *json;
	int ret;

	ret = read_file(oracle_path, &data, &len);
	if (ret)
		return ret;

	json = realloc(data, len + 1);
	if (!json) {
		free(data);
		return -ENOMEM;
	}
	json[len] = '\0';

	ret = 0;
	if (!json_number(json, "sent", &o->sent) || !json_number(json, "dropped", &o->dropped) ||
			!json_number(json, "reordered", &o->reordered) ||
			!json_number(json, "duplicated", &o->duplicated))
		ret = -EBADMSG;
	free(json);

	if (!ret)
		printf("  oracle: %llu sent, %llu dropped, %llu reordered, %llu duplicated\n",
				o->sent, o->dropped, o->reordered, o->duplicated);

	return ret;
}

/* The setups and starts the emulator logged (udp-stream.c) */
static void streams_read(struct streams *st)
{
	const char *path = getenv("IIOD_EMU_LOG");
	unsigned int id, payload;
	char line[512];
	FILE *f;

	memset(st, 0, sizeof(*st));
	f = path ? fopen(path, "r") : NULL;
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "UDP stream %8x started", &id) == 1 && strstr(line, " started")) {
			if (st->nb_started < MAX_STREAMS)
				st->started[st->nb_started++] = id;
		} else if (sscanf(line, "UDP stream %8x: %*s buffer %*u, %*s to %*s %u-byte", &id,
					   &payload) == 2) {
			if (st->nb_setup < MAX_STREAMS) {
				st->payload[st->nb_setup] = payload;
				st->setup[st->nb_setup++] = id;
			}
		}
	}
	fclose(f);
}

static long long attr_ll(const char *name)
{
	const struct iio_attr *attr = iio_buffer_find_attr(lb.buf, name);
	long long val;

	if (!attr || iio_attr_read_longlong(attr, &val))
		return -1;

	return val;
}

static const char *attr_str(const char *name)
{
	static char val[64];
	const struct iio_attr *attr = iio_buffer_find_attr(lb.buf, name);
	ssize_t ret;

	if (!attr)
		return "(missing)";

	ret = iio_attr_read_raw(attr, val, sizeof(val));
	if (ret < 0)
		snprintf(val, sizeof(val), "(error %zd)", ret);

	return val;
}

static void counters_read(struct counters *c)
{
	c->packets = attr_ll("daqiri_rx_packets");
	c->bytes = attr_ll("daqiri_rx_bytes");
	c->lost = attr_ll("daqiri_rx_lost_packets");
	c->gaps = attr_ll("daqiri_rx_gap_events");
	c->dropped_no_block = attr_ll("daqiri_rx_dropped_no_block");
	c->overruns = attr_ll("daqiri_rx_overrun_events");
	c->foreign = attr_ll("daqiri_rx_foreign_packets");
	c->malformed = attr_ll("daqiri_rx_malformed");
	c->zero_filled = attr_ll("daqiri_rx_zero_filled_bytes");
	c->late = attr_ll("daqiri_rx_late_packets");
	c->duplicates = attr_ll("daqiri_rx_duplicate_packets");
	c->completed = attr_ll("daqiri_blocks_completed");
	c->failed = attr_ll("daqiri_blocks_failed");
	c->with_gaps = attr_ll("daqiri_blocks_with_gaps");
	c->last_error = attr_ll("daqiri_last_error");
}

static void lb_close(void)
{
	while (lb.nb_blocks)
		iio_block_destroy(lb.blocks[--lb.nb_blocks]);
	if (lb.stream)
		iio_buffer_close(lb.stream);
	if (lb.mask)
		iio_channels_mask_destroy(lb.mask);
	if (lb.ctx)
		iio_context_destroy(lb.ctx);

	memset(&lb, 0, sizeof(lb));
}

/* Context with <extra> URI options, buffer with NB_BLOCKS blocks */
static int lb_open(const char *extra)
{
	static const char *const chans[] = { "voltage0", "voltage1" };
	const struct iio_device *dev;
	struct iio_channel *chn;
	struct iio_block *block;
	char uri[4300];
	unsigned int i;
	int ret;

	memset(&lb, 0, sizeof(lb));

	snprintf(uri, sizeof(uri), "daqiri:127.0.0.1,data=udp,yaml=%s%s", yaml, extra);
	lb.ctx = iio_create_context(NULL, uri);
	ret = iio_err(lb.ctx);
	if (ret) {
		printf("  %s: unable to create context: %d\n", uri, ret);
		lb.ctx = NULL;
		return ret;
	}

	ret = iio_context_set_timeout(lb.ctx, DEQUEUE_TIMEOUT_MS);
	if (ret)
		return ret;

	dev = iio_context_find_device(lb.ctx, DEV_NAME);
	if (!dev)
		return -ENODEV;

	lb.mask = iio_create_channels_mask(iio_device_get_channels_count(dev));
	if (!lb.mask)
		return -ENOMEM;

	for (i = 0; i < 2; i++) {
		chn = iio_device_find_channel(dev, chans[i], false);
		if (!chn)
			return -ENOENT;
		iio_channel_enable(chn, lb.mask);
	}

	lb.buf = iio_device_get_buffer(dev, 0);
	if (!lb.buf)
		return -ENODEV;

	lb.stream = iio_buffer_open(lb.buf, lb.mask);
	ret = iio_err(lb.stream);
	if (ret) {
		lb.stream = NULL;
		return ret;
	}

	while (lb.nb_blocks < NB_BLOCKS) {
		block = iio_buffer_stream_create_block(lb.stream, PAYLOAD);
		ret = iio_err(block);
		if (ret)
			return ret;
		lb.blocks[lb.nb_blocks++] = block;
	}

	return 0;
}

static int lb_enqueue(unsigned int first, unsigned int nb)
{
	unsigned int i;
	int ret;

	for (i = first; i < first + nb; i++) {
		ret = iio_block_enqueue(lb.blocks[i % NB_BLOCKS], 0, false);
		if (ret)
			return ret;
	}

	return 0;
}

/* A fresh buffer (lossy=1 with <lossy>), <queued> blocks queued, enabled */
static int lb_start(bool lossy, unsigned int queued)
{
	int ret;

	/* What is there after the disable comes from this case */
	remove(oracle_path);

	ret = lb_open(lossy ? ",lossy=1" : "");
	if (!ret)
		ret = lb_enqueue(0, queued);
	if (!ret)
		ret = iio_buffer_stream_start(lb.stream);

	return ret;
}

static void lb_record(long rec)
{
	if (lb.nb_recs < MAX_RECS)
		lb.recs[lb.nb_recs++] = rec;
}

/* Dequeue the next block, waiting for it unless <nonblock>, and record what
 * it holds; queue it again with <requeue>. -EBUSY or -EPERM: the block is
 * still queued, or was not, and nothing is recorded. */
static int lb_take(bool nonblock, bool requeue)
{
	struct iio_block *b = lb.blocks[lb.next];
	int ret;

	ret = iio_block_dequeue(b, nonblock);
	if (nonblock && (ret == -EBUSY || ret == -EPERM))
		return ret;

	if (ret == -EIO) {
		lb_record(BLK_EIO);
	} else if (ret) {
		printf("  dequeue of block %u: %d\n", lb.nb_recs, ret);
		lb_record(BLK_ERR);
		return ret;
	} else {
		lb_record(classify(b));
	}

	lb.next = (lb.next + 1) % NB_BLOCKS;

	return requeue ? iio_block_enqueue(b, 0, false) : 0;
}

/* Dequeue <nb> blocks, each queued again; false on an error */
static bool lb_run(unsigned int nb)
{
	unsigned int i;
	int ret;

	for (i = 0; i < nb; i++) {
		ret = lb_take(false, true);
		if (ret) {
			printf("  stream stopped after %u blocks: %d\n", lb.nb_recs, ret);
			return false;
		}
	}

	return true;
}

/* Disable, then dequeue the blocks that completed meanwhile; false on an
 * error */
static bool lb_stop(void)
{
	int ret;

	ret = iio_buffer_stream_stop(lb.stream);
	CHECK_EQ(ret, 0, "disabled");
	if (ret)
		return false;

	while (!(ret = lb_take(true, false)))
		;

	return ret == -EBUSY || ret == -EPERM;
}

/* Datagrams the backend has seen: delivered or not, late or duplicate */
static long long arrived(const struct counters *c)
{
	return c->packets + c->late + c->duplicates;
}

static bool dropped_by(const struct inject *inj, long seq)
{
	unsigned int i;

	for (i = 0; i < inj->nb_drop; i++)
		if (inj->drop_list[i] == seq)
			return true;

	return false;
}

static unsigned int emit(const struct inject *inj, long seq, long *order, unsigned int n)
{
	if (n < MAX_RECS)
		order[n++] = seq;
	if (inj->dup_every && (seq + 1) % inj->dup_every == 0 && n < MAX_RECS)
		order[n++] = seq;

	return n;
}

/*
 * The first <sent> datagrams the emulator sends with <inj>, in order (its
 * generator, iiod-emu/udp-stream.c): the dropped sequence numbers skipped; for
 * a reorder window k, datagram mk sent just before mk-1 unless one of them is
 * dropped; with dup_every n, datagrams n-1, 2n-1, ... sent twice in a row.
 * Pairs and copies go out whole. Returns the count, or 0 when <sent> ends in
 * the middle of one.
 */
static unsigned int emission(const struct inject *inj, unsigned long long sent, long *order)
{
	long seq = 0, k = inj->reorder_window;
	unsigned int n = 0;

	while (n < sent && n < MAX_RECS) {
		if (dropped_by(inj, seq)) {
			seq++;
		} else if (k && seq % k == k - 1 && !dropped_by(inj, seq + 1)) {
			n = emit(inj, seq + 1, order, n);
			n = emit(inj, seq, order, n);
			seq += 2;
		} else {
			n = emit(inj, seq, order, n);
			seq++;
		}
	}

	return n == sent ? n : 0;
}

/* What the host makes of datagrams arriving in <order> */
struct expect {
	long blocks[MAX_RECS];
	unsigned int nb_blocks;
	long long packets, lost, gaps, late, duplicates, eio, zeros, last_seq;
};

static void expect_blocks(const long *order, unsigned int nb, bool lossy, struct expect *e)
{
	static bool seen[MAX_SEQ];
	long p, g, last = -1;
	unsigned int i;

	memset(e, 0, sizeof(*e));
	memset(seen, 0, sizeof(seen));

	for (i = 0; i < nb; i++) {
		p = order[i];
		if (p < 0 || p >= MAX_SEQ)
			continue;

		if (p <= last) {
			/* The last accepted one again, or an older one */
			if (seen[p])
				e->duplicates++;
			else
				e->late++;
			seen[p] = true;
			continue;
		}

		if (p > last + 1) {
			e->gaps++;
			if (!lossy && e->nb_blocks < MAX_RECS) {
				e->blocks[e->nb_blocks++] = BLK_EIO;
				e->eio++;
			}
			for (g = p - last - 1; lossy && g && e->nb_blocks < MAX_RECS; g--) {
				e->blocks[e->nb_blocks++] = BLK_ZERO;
				e->zeros++;
			}
		}

		if (e->nb_blocks < MAX_RECS)
			e->blocks[e->nb_blocks++] = p;
		e->packets++;
		seen[p] = true;
		last = p;
	}

	/* Sequence numbers up to the last one that never arrived */
	e->last_seq = last;
	e->lost = last + 1 - e->packets - e->late;
}

/* The blocks dequeued are exactly <exp> */
static void check_blocks(const struct expect *e)
{
	unsigned int i, bad = 0;
	char got[32], want[32];

	CHECK_EQ(lb.nb_recs, e->nb_blocks, "blocks dequeued: as the emulator's order gives");
	for (i = 0; i < lb.nb_recs && i < e->nb_blocks; i++) {
		if (lb.recs[i] == e->blocks[i])
			continue;
		if (bad++ < 4)
			printf("  block %u: %s, expected %s\n", i,
					blk_name(lb.recs[i], got, sizeof(got)),
					blk_name(e->blocks[i], want, sizeof(want)));
	}
	CHECK_EQ(bad, 0, "every block as expected, its bytes those of the sample file");
}

/* One enable: one probe at open (whose stream never starts), then one setup
 * and one start, of the stream daqiri_stream_id names */
static void check_streams(unsigned int enables)
{
	char id[16];
	struct streams st;
	unsigned int i;
	bool ok;

	streams_read(&st);
	CHECK_EQ(st.nb_setup, 1 + enables, "emulator: a setup at open and one per enable");
	CHECK_EQ(st.nb_started, enables, "emulator: one stream started per enable");
	if (st.nb_setup != 1 + enables || st.nb_started != enables)
		return;

	for (ok = true, i = 0; i < st.nb_setup; i++)
		ok = ok && st.payload[i] == PAYLOAD;
	TEST_ASSERT(ok, "emulator: 9200-byte payloads");

	for (ok = true, i = 0; i < enables; i++)
		ok = ok && st.started[i] == st.setup[i + 1] && st.started[i] != st.setup[0];
	TEST_ASSERT(ok, "the streams of the enables' setups start, the probe's never");

	snprintf(id, sizeof(id), "0x%08" PRIx32, st.started[enables - 1]);
	TEST_ASSERT_STR_EQ(
			attr_str("daqiri_stream_id"), id, "daqiri_stream_id: the last one started");
}

/*
 * The counters against the oracle and the model of the blocks: the checks of
 * every case with injection only, with <e> the host's view of the emission
 * order. First that the drain after the disable saw everything sent: if not,
 * nothing else can match.
 */
static void check_counters(const struct oracle *o, const struct expect *e)
{
	long long trailing, originals;
	struct counters c;
	long last = -1;
	unsigned int i;

	counters_read(&c);

	CHECK_EQ(arrived(&c), o->sent,
			"every datagram sent arrived: rx_packets + late + duplicates");
	if (arrived(&c) != (long long)o->sent)
		printf("  the drain after the disable missed %lld of %llu datagrams\n",
				(long long)o->sent - arrived(&c), o->sent);

	CHECK_EQ(c.duplicates, o->duplicated, "duplicate_packets == oracle duplicated");
	CHECK_EQ(c.late, o->reordered, "late_packets == oracle reordered");

	/* The last datagram the host received, from the blocks */
	for (i = 0; i < lb.nb_recs; i++)
		if (lb.recs[i] > last)
			last = lb.recs[i];
	CHECK_EQ(last, e->last_seq, "the last datagram received is the last one sent");

	/* 0 .. last were sent once or dropped; the drops past last are the
	 * trailing ones */
	originals = (long long)o->sent - (long long)o->duplicated;
	trailing = originals + (long long)o->dropped - 1 - last;
	printf("  last datagram %ld, %lld trailing drops\n", last, trailing);
	TEST_ASSERT(trailing >= 0, "oracle and blocks agree on the datagrams sent");
	CHECK_EQ(c.lost, (long long)o->dropped - trailing,
			"lost_packets == oracle dropped less the trailing drops");

	CHECK_EQ(c.packets, e->packets, "rx_packets: accepted datagrams");
	CHECK_EQ(c.bytes, c.packets * PAYLOAD, "rx_bytes: their payloads");
	CHECK_EQ(c.lost, e->lost, "lost_packets: as the emission order gives");
	CHECK_EQ(c.gaps, e->gaps, "gap_events");
	CHECK_EQ(c.zero_filled, e->zeros * PAYLOAD, "zero_filled_bytes: exactly the lost payloads");
	CHECK_EQ(c.completed, (long long)e->nb_blocks - e->eio, "blocks_completed");
	CHECK_EQ(c.failed, e->eio, "blocks_failed: the -EIO blocks");
	CHECK_EQ(c.with_gaps, e->eio + e->zeros, "blocks_with_gaps");
	CHECK_EQ(c.last_error, e->eio ? -EIO : 0, "last_error");
	CHECK_EQ(c.dropped_no_block, 0, "dropped_no_block: blocks always queued");
	CHECK_EQ(c.overruns, 0, "overrun_events");
	CHECK_EQ(c.foreign, 0, "foreign_packets");
	CHECK_EQ(c.malformed, 0, "malformed");
}

/* Stream, stop, then check blocks and counters against the model of what
 * the emulator sent with <inj> */
static void injection_case(const struct inject *inj, bool lossy, struct oracle *o)
{
	static long order[MAX_RECS];
	static struct expect e;
	unsigned int nb;
	int ret;

	memset(o, 0, sizeof(*o));

	ret = lb_start(lossy, NB_BLOCKS);
	CHECK_EQ(ret, 0,
			lossy ? "lossy=1 buffer enabled with 64 blocks queued"
			      : "buffer enabled with 64 blocks queued");
	if (ret)
		return;
	TEST_ASSERT_STR_EQ(attr_str("daqiri_data_path"), "udp", "data path");

	if (!lb_run(RUN_BLOCKS) || !lb_stop())
		return;

	ret = oracle_read(o);
	CHECK_EQ(ret, 0, "oracle written at the disable");
	if (ret)
		return;

	nb = emission(inj, o->sent, order);
	TEST_ASSERT(nb > 0, "the oracle's datagrams are whole pairs and copies");
	if (!nb)
		return;

	expect_blocks(order, nb, lossy, &e);
	check_blocks(&e);
	check_counters(o, &e);
	check_streams(1);
}

/* Nothing injected: block k is datagram k */
TEST_FUNCTION(clean)
{
	const struct inject inj = { 0 };
	struct oracle o;

	injection_case(&inj, false, &o);
	CHECK_EQ(o.dropped + o.reordered + o.duplicated, 0, "oracle: nothing injected");
	TEST_ASSERT(o.sent >= RUN_BLOCKS, "oracle: at least 64 sent");
}

static const long drops[] = { 5, 6, 20 };

/* lossy=0: the gap of 5 and 6 fails one block, that of 20 another */
TEST_FUNCTION(drop_list)
{
	const struct inject inj = { drops, 3, 0, 0 };
	struct oracle o;

	injection_case(&inj, false, &o);
	CHECK_EQ(o.dropped, 3, "oracle: 5, 6 and 20 dropped");
	if (lb.nb_recs > 20) {
		CHECK_EQ(lb.recs[5], BLK_EIO, "block 5: -EIO, datagrams 5 and 6 lost");
		CHECK_EQ(lb.recs[6], 7, "block 6: datagram 7");
		CHECK_EQ(lb.recs[19], BLK_EIO, "block 19: -EIO, datagram 20 lost");
		CHECK_EQ(lb.recs[20], 21, "block 20: datagram 21");
	}
}

/* lossy=1: blocks 5, 6 and 20 are all zero, every other block k datagram k */
TEST_FUNCTION(drop_list_lossy)
{
	const struct inject inj = { drops, 3, 0, 0 };
	struct oracle o;

	injection_case(&inj, true, &o);
	CHECK_EQ(o.dropped, 3, "oracle: 5, 6 and 20 dropped");
	if (lb.nb_recs > 21) {
		CHECK_EQ(lb.recs[4], 4, "block 4: datagram 4");
		CHECK_EQ(lb.recs[5], BLK_ZERO, "block 5: zeros for datagram 5");
		CHECK_EQ(lb.recs[6], BLK_ZERO, "block 6: zeros for datagram 6");
		CHECK_EQ(lb.recs[7], 7, "block 7: datagram 7, the stream in place");
		CHECK_EQ(lb.recs[20], BLK_ZERO, "block 20: zeros for datagram 20");
		CHECK_EQ(lb.recs[21], 21, "block 21: datagram 21");
	}
}

/* --udp-reorder-window 3: 3 before 2, 6 before 5, ... Each pair fails the
 * block of the late datagram's place (a gap of one, then the late one takes
 * the loss back): block k is datagram k, or -EIO for k = 3m + 2 */
TEST_FUNCTION(reorder)
{
	const struct inject inj = { NULL, 0, 3, 0 };
	unsigned int i, ok = 0;
	struct oracle o;

	injection_case(&inj, false, &o);
	TEST_ASSERT(o.reordered >= 20, "oracle: at least 20 pairs reordered");
	CHECK_EQ(o.dropped + o.duplicated, 0, "oracle: nothing dropped or duplicated");
	for (i = 0; i < lb.nb_recs; i++)
		ok += lb.recs[i] == (i % 3 == 2 ? BLK_EIO : (long)i);
	CHECK_EQ(ok, lb.nb_recs, "block k: -EIO where k = 3m + 2, datagram k elsewhere");
}

/* --udp-dup-every 8: 7, 15, 23, ... sent twice; the copies are dropped */
TEST_FUNCTION(duplicate)
{
	const struct inject inj = { NULL, 0, 0, 8 };
	unsigned int i, ok = 0;
	struct oracle o;

	injection_case(&inj, false, &o);
	CHECK_EQ(o.dropped + o.reordered, 0, "oracle: nothing dropped or reordered");
	if (lb.nb_recs)
		CHECK_EQ(o.duplicated, (lb.recs[lb.nb_recs - 1] + 1) / 8,
				"oracle: one copy of every 8th datagram sent");
	for (i = 0; i < lb.nb_recs; i++)
		ok += lb.recs[i] == (long)i;
	CHECK_EQ(ok, lb.nb_recs, "block k is datagram k: no copy delivered");
}

/*
 * A host overrun at --udp-pps 100: two blocks queued and not dequeued, so
 * datagrams 0 and 1 fill them and the next ones find no block. Once three are
 * dropped, the 62 other blocks are queued, then the first two dequeued. The
 * next datagram that finds a block, F, follows a gap of unknown size: with
 * lossy=0 that block fails with -EIO and F fills the next; with lossy=1 F
 * fills it, flagged, not zero-filled. Blocks then follow F in order. Nothing
 * is lost on the wire, so every datagram from 2 to F - 1 was dropped for want
 * of a block, one overrun.
 */
static void slow_consumer_case(bool lossy)
{
	unsigned int i, f_at = lossy ? 2 : 3, ok = 0;
	struct counters c;
	struct timespec t0;
	struct oracle o;
	long f;
	int ret;

	ret = lb_start(lossy, 2);
	CHECK_EQ(ret, 0, "buffer enabled with 2 blocks queued");
	if (ret)
		return;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (attr_ll("daqiri_rx_dropped_no_block") < 3 && ms_since(&t0) < WAIT_TIMEOUT_MS)
		sleep_ms(1);
	TEST_ASSERT(attr_ll("daqiri_rx_dropped_no_block") >= 3, "datagrams dropped: no block");
	CHECK_EQ(attr_ll("daqiri_rx_overrun_events"), 1, "one overrun event, not one per datagram");

	ret = lb_enqueue(2, NB_BLOCKS - 2);
	CHECK_EQ(ret, 0, "the 62 other blocks queued");
	if (ret || lb_take(false, true) || lb_take(false, true) || !lb_run(16) || !lb_stop())
		return;

	ret = oracle_read(&o);
	CHECK_EQ(ret, 0, "oracle written at the disable");
	if (ret)
		return;
	CHECK_EQ(o.dropped + o.reordered + o.duplicated, 0, "oracle: nothing injected");

	TEST_ASSERT(lb.nb_recs > f_at + 1, "blocks after the overrun");
	if (lb.nb_recs <= f_at + 1)
		return;

	CHECK_EQ(lb.recs[0], 0, "block 0: datagram 0");
	CHECK_EQ(lb.recs[1], 1, "block 1: datagram 1");
	if (!lossy)
		CHECK_EQ(lb.recs[2], BLK_EIO, "block 2: -EIO, the overrun");
	f = lb.recs[f_at];
	printf("  first datagram after the overrun: %ld\n", f);
	TEST_ASSERT(f > 4, "datagrams after the first ones dropped");
	for (i = f_at; i < lb.nb_recs; i++)
		ok += lb.recs[i] == f + (long)(i - f_at);
	CHECK_EQ(ok, lb.nb_recs - f_at, "then every datagram from F on, in order");

	counters_read(&c);
	CHECK_EQ(arrived(&c), o.sent, "every datagram sent arrived");
	CHECK_EQ(c.packets, o.sent, "rx_packets: none lost, late or duplicate");
	CHECK_EQ(c.lost, 0, "lost_packets: nothing lost on the wire");
	CHECK_EQ(c.gaps, 0, "gap_events: no sequence gap");
	CHECK_EQ(c.dropped_no_block, f - 2, "dropped_no_block: datagrams 2 to F - 1");
	CHECK_EQ(c.dropped_no_block + lb.nb_recs - (lossy ? 0 : 1), c.packets,
			"each datagram dropped or in a block");
	CHECK_EQ(c.overruns, 1, "overrun_events: one");
	CHECK_EQ(c.zero_filled, 0, "zero_filled_bytes: an overrun is not filled");
	CHECK_EQ(c.completed, lb.nb_recs - (lossy ? 0 : 1), "blocks_completed");
	CHECK_EQ(c.failed, lossy ? 0 : 1, "blocks_failed: the -EIO block");
	CHECK_EQ(c.with_gaps, 1, "blocks_with_gaps: the overrun's block");
	CHECK_EQ(c.last_error, lossy ? 0 : -EIO, "last_error");
	check_streams(1);
}

TEST_FUNCTION(slow_consumer)
{
	slow_consumer_case(false);
}

TEST_FUNCTION(slow_consumer_lossy)
{
	slow_consumer_case(true);
}

/*
 * Two bursts with 500 ms of silence in between: enable, 32 blocks, disable,
 * wait, enable again, 32 blocks, disable. The second enable has a setup of its
 * own: another stream ID, the sequence from 0, and the payloads from the start
 * of the file again. The blocks still queued at the first disable take the
 * second burst; the counters add up over both.
 */
TEST_FUNCTION(burst_gap)
{
	unsigned int i, first, ok = 0;
	char id1[16] = "";
	struct oracle o1, o2;
	struct counters c;
	int ret;

	ret = lb_start(false, NB_BLOCKS);
	CHECK_EQ(ret, 0, "buffer enabled with 64 blocks queued");
	if (ret || !lb_run(32) || !lb_stop())
		return;

	/* The emulator rewrites the oracle at every disable */
	ret = oracle_read(&o1);
	CHECK_EQ(ret, 0, "oracle of the first burst");
	if (ret)
		return;
	first = lb.nb_recs;
	CHECK_EQ(first, o1.sent, "first burst: one block per datagram sent");
	snprintf(id1, sizeof(id1), "%s", attr_str("daqiri_stream_id"));

	sleep_ms(500);

	ret = iio_buffer_stream_start(lb.stream);
	CHECK_EQ(ret, 0, "enabled again after 500 ms");
	if (ret || !lb_run(32) || !lb_stop())
		return;

	ret = oracle_read(&o2);
	CHECK_EQ(ret, 0, "oracle of the second burst");
	if (ret)
		return;
	CHECK_EQ(o1.dropped + o1.reordered + o1.duplicated + o2.dropped + o2.reordered +
					o2.duplicated,
			0, "oracles: nothing injected");
	CHECK_EQ(lb.nb_recs - first, o2.sent, "second burst: one block per datagram sent");
	TEST_ASSERT(strcmp(attr_str("daqiri_stream_id"), id1), "a new stream ID");

	for (i = 0; i < lb.nb_recs; i++)
		ok += lb.recs[i] == (long)(i < first ? i : i - first);
	CHECK_EQ(ok, lb.nb_recs, "each burst: datagrams 0, 1, 2, ... in order");

	counters_read(&c);
	CHECK_EQ(c.packets, o1.sent + o2.sent, "rx_packets: both bursts");
	CHECK_EQ(c.lost, 0, "lost_packets: the sequence restart is no gap");
	CHECK_EQ(c.gaps, 0, "gap_events");
	CHECK_EQ(c.late + c.duplicates, 0, "nothing late or duplicate");
	CHECK_EQ(c.completed, lb.nb_recs, "blocks_completed");
	CHECK_EQ(c.failed + c.dropped_no_block, 0, "no block failed, nothing dropped");
	check_streams(2);
}

struct lb_case {
	const char *name;
	void (*run)(void);
	/* The emulator's options after --udp */
	const char *const flags[6];
};

static const struct lb_case cases[] = {
	{ "clean", test_clean, { "--udp-pps", "1000" } },
	{ "drop_list", test_drop_list, { "--udp-pps", "1000", "--udp-drop-list", "5,6,20" } },
	{ "drop_list_lossy", test_drop_list_lossy,
			{ "--udp-pps", "1000", "--udp-drop-list", "5,6,20" } },
	{ "reorder", test_reorder, { "--udp-pps", "1000", "--udp-reorder-window", "3" } },
	{ "duplicate", test_duplicate, { "--udp-pps", "1000", "--udp-dup-every", "8" } },
	{ "slow_consumer", test_slow_consumer, { "--udp-pps", "100" } },
	{ "slow_consumer_lossy", test_slow_consumer_lossy, { "--udp-pps", "100" } },
	{ "burst_gap", test_burst_gap, { "--udp-pps", "1000" } },
};

#define NB_CASES (sizeof(cases) / sizeof(cases[0]))

/* One case, against the emulator with_emu.sh started for it */
static int run_case(const char *name)
{
	unsigned int i;
	int ret;

	yaml = getenv("DAQIRI_YAML");
	if (!yaml || !yaml[0]) {
		printf("DAQIRI_YAML must be the path of socket-udp-loopback.yaml\n");
		return 1;
	}

	for (i = 0; i < NB_CASES && strcmp(cases[i].name, name); i++)
		;
	if (i == NB_CASES) {
		printf("No case %s\n", name);
		return 2;
	}

	ret = map_offsets();
	if (ret) {
		printf("The sample file cannot tell %d datagrams apart: %d\n", MAX_SEQ, ret);
		return 1;
	}

	printf("Running test: %s\n", name);
	cases[i].run();
	lb_close();

	printf("test_udp_loopback --case %s: %d/%d passed, %d failed\n", name, test_passed,
			test_count, test_failed);

	return test_failed ? 1 : 0;
}

/* Each case in a process of its own, under with_emu.sh with its flags */
static int run_all(const char *with_emu, const char *xml, const char *sample)
{
	const char *argv[32];
	unsigned int i, j, n, failed = 0;
	char self[4096];
	ssize_t len;
	int status;
	pid_t pid;

	len = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (len < 0) {
		perror("/proc/self/exe");
		return 1;
	}
	self[len] = '\0';

	for (i = 0; i < NB_CASES; i++) {
		n = 0;
		argv[n++] = with_emu;
		argv[n++] = "--emu-args";
		argv[n++] = "--udp";
		for (j = 0; j < sizeof(cases[i].flags) / sizeof(cases[i].flags[0]) &&
				cases[i].flags[j];
				j++)
			argv[n++] = cases[i].flags[j];
		argv[n++] = "--udp-json";
		argv[n++] = oracle_path;
		argv[n++] = "--";
		argv[n++] = xml;
		argv[n++] = self;
		argv[n++] = "--case";
		argv[n++] = cases[i].name;
		argv[n++] = sample;
		argv[n++] = oracle_path;
		argv[n] = NULL;

		printf("== %s\n", cases[i].name);
		fflush(stdout);

		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (!pid) {
			execv(with_emu, (char *const *)argv);
			_exit(127);
		}

		while (waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR) {
				perror("waitpid");
				return 1;
			}
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			printf("== %s FAILED\n", cases[i].name);
			failed++;
		}
	}

	printf("test_udp_loopback: %u/%u cases passed\n", (unsigned int)NB_CASES - failed,
			(unsigned int)NB_CASES);

	return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
	int ret;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc == 5 && !strcmp(argv[1], "--case")) {
		oracle_path = argv[4];
		ret = read_file(argv[3], &file, &file_len);
		if (ret || !file_len) {
			printf("Unable to read the sample file %s: %d\n", argv[3], ret);
			return 1;
		}

		ret = run_case(argv[2]);
		free(seq_at);
		free(file);
		return ret;
	}

	if (argc != 5) {
		printf("Usage: %s <with_emu.sh> <xml> <sample.bin> <oracle.json>\n"
		       "       %s --case <name> <sample.bin> <oracle.json>\n",
				argv[0], argv[0]);
		return 2;
	}

	oracle_path = argv[4];

	return run_all(argv[1], argv[2], argv[3]);
}
