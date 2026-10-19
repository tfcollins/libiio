/* SPDX-License-Identifier: MIT */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * End-to-end test of daqiri:<host>,framing=vrt over the DAQIRI socket engine.
 * vrt_sender (path in VRT_SENDER) sends VITA 49.2 IF data from 127.0.0.1:25102
 * to the engine on 127.0.0.1:25101 (DAQIRI_YAML: socket-udp-loopback.yaml),
 * against iiod-emu serving fmcomms2.xml.
 *
 * cf-ad9361-lpc voltage0+voltage1 is 4 bytes per sample, swapped from the
 * big-endian wire to host order. Blocks are 1440 bytes, one 360-sample packet
 * each, so every lost packet is one failed (lossy=0) or one zero-filled
 * (lossy=1) block.
 *
 * Each case opens its own context and buffer (fresh counters), queues the
 * blocks, starts the stream and only then forks the sender: a stream drops
 * whatever arrived before it started. It dequeues until the block holding
 * the last packet, waits for the sender to exit, reads its --json oracle and
 * waits until the backend has counted every datagram the oracle says was
 * sent. Only then does it check the counters and close, so no packet of one
 * case reaches the next.
 *
 * The expected blocks follow from the oracle's emission order (see
 * vrt_sender --help): a packet at or below the last delivered one is skipped
 * (late or duplicate); a gap before a packet fails one block with lossy=0,
 * and zero-fills one block per lost packet with lossy=1.
 *
 * The blocks are driven with iio_buffer_open() and enqueue/dequeue rather
 * than iio_stream: iio_stream_get_next_block() enables the buffer inside its
 * first, blocking call, so the sender could not be started after enable.
 */

#include <ctype.h>
#include <errno.h>
#include <iio/iio.h>
#include <signal.h>
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

/* voltage0+voltage1: 4 bytes per sample, one 360-sample packet per block */
#define SPP 360
#define BLOCK_SIZE (SPP * 4)
#define NB_BLOCKS 8

/* The YAML's local_addr; vrt_sender binds its remote_addr by default */
#define DEST "127.0.0.1:25101"

#define DEQUEUE_TIMEOUT_MS 3000
#define SETTLE_TIMEOUT_MS 3000
#define SENDER_TIMEOUT_MS 10000

#define MAX_RECS 512
#define ORACLE_MAX 512
#define ORACLE_BYTES 65536

/* A block as dequeued, or as expected: a packet index (>= 0) or one of */
#define BLK_EIO (-1)  /* dequeue returned -EIO */
#define BLK_ZERO (-2) /* all zero */
#define BLK_BAD (-3)  /* neither all zero nor one whole ramp16 packet */
#define BLK_ERR (-4)  /* any other error; ends the stream */

/* CHECK_EQ() evaluates <actual> again to print it: evaluate once */
#define CHECK_EQ(actual, expected, message) \
	do { \
		long check_val_ = (long)(actual); \
		TEST_ASSERT_EQ(check_val_, (long)(expected), message); \
	} while (0)

static const char *yaml, *sender_path;
static char json_path[4096];

struct oracle {
	long sent; /* datagrams on the wire, duplicates included */
	unsigned int nb_dropped, nb_reordered, nb_duplicated;
	long dropped[ORACLE_MAX];
	long reordered[ORACLE_MAX][2];
	long duplicated[ORACLE_MAX];
};

struct lb_case {
	struct iio_context *ctx;
	struct iio_buffer *buf;
	struct iio_channels_mask *mask;
	struct iio_buffer_stream *stream;
	struct iio_block *blocks[NB_BLOCKS];
	unsigned int nb_blocks;
	unsigned int next; /* next block to dequeue */
	pid_t sender;
	struct timespec t_start; /* just before the sender was forked */
	long recs[MAX_RECS];
	unsigned int nb_recs;
	struct oracle o;
};

/* One at a time; too large for the stack */
static struct lb_case lc;

static double ms_since(const struct timespec *t0)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (double)(now.tv_sec - t0->tv_sec) * 1e3 + (double)(now.tv_nsec - t0->tv_nsec) / 1e6;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static long long attr_ll(const struct iio_buffer *buf, const char *name)
{
	const struct iio_attr *attr = iio_buffer_find_attr(buf, name);
	long long val;

	if (!attr || iio_attr_read_longlong(attr, &val))
		return -1;

	return val;
}

/* ramp16 (gen_golden.py): sample n has I = n & 0x7FFF, Q = (0x8000 - n) &
 * 0xFFFF; after swap16 both are host-order 16-bit words, I first */
static bool samples_are_ramp(const uint8_t *p, unsigned long first)
{
	unsigned long n;
	uint16_t i, q;
	unsigned int k;

	for (k = 0; k < SPP; k++) {
		n = first + k;
		memcpy(&i, p + 4 * k, 2);
		memcpy(&q, p + 4 * k + 2, 2);
		if (i != (uint16_t)(n & 0x7FFF) || q != (uint16_t)((0x8000 - n) & 0xFFFF))
			return false;
	}

	return true;
}

/* BLK_ZERO, the index of the ramp16 packet the block holds (unambiguous
 * below 32768 samples, i.e. 91 packets), or BLK_BAD */
static long classify(const struct iio_block *block)
{
	const uint8_t *p = iio_block_start(block);
	unsigned int k;
	uint16_t i;

	for (k = 0; k < BLOCK_SIZE && !p[k]; k++)
		;
	if (k == BLOCK_SIZE)
		return BLK_ZERO;

	memcpy(&i, p, 2);
	if (i % SPP || !samples_are_ramp(p, i))
		return BLK_BAD;

	return i / SPP;
}

static const char *skip_ws(const char *p)
{
	while (isspace((unsigned char)*p))
		p++;

	return p;
}

/* The value of "key" in a flat JSON object, or NULL */
static const char *json_value(const char *s, const char *key)
{
	char pat[32];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(s, pat);
	if (!p)
		return NULL;

	p = skip_ws(p + strlen(pat));
	if (*p != ':')
		return NULL;

	return skip_ws(p + 1);
}

static const char *json_long(const char *p, long *val)
{
	char *end;

	errno = 0;
	*val = strtol(p, &end, 10);
	if (end == p || errno)
		return NULL;

	return end;
}

/* [n, ...] (width 1) or [[a, b], ...] (width 2) of integers into out */
static int json_array(const char *p, long *out, unsigned int width, unsigned int max,
		unsigned int *nb)
{
	unsigned int w;

	*nb = 0;
	if (!p || *p++ != '[')
		return -EINVAL;

	p = skip_ws(p);
	if (*p == ']')
		return 0;

	for (;;) {
		if (*nb == max)
			return -E2BIG;
		if (width > 1 && *p++ != '[')
			return -EINVAL;

		for (w = 0; w < width; w++) {
			p = json_long(skip_ws(p), &out[*nb * width + w]);
			if (!p)
				return -EINVAL;
			p = skip_ws(p);
			if (w + 1 < width && *p++ != ',')
				return -EINVAL;
		}

		if (width > 1 && *p++ != ']')
			return -EINVAL;
		(*nb)++;

		p = skip_ws(p);
		if (*p == ']')
			return 0;
		if (*p++ != ',')
			return -EINVAL;
		p = skip_ws(p);
	}
}

static int oracle_read(const char *path, struct oracle *o)
{
	static char text[ORACLE_BYTES];
	const char *p;
	size_t len;
	FILE *f;
	int ret;

	memset(o, 0, sizeof(*o));

	f = fopen(path, "r");
	if (!f)
		return -errno;
	len = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	if (len == sizeof(text) - 1)
		return -E2BIG;
	text[len] = '\0';

	p = json_value(text, "sent");
	if (!p || !json_long(p, &o->sent))
		return -EINVAL;

	ret = json_array(json_value(text, "dropped"), o->dropped, 1, ORACLE_MAX, &o->nb_dropped);
	if (!ret)
		ret = json_array(json_value(text, "reordered"), &o->reordered[0][0], 2, ORACLE_MAX,
				&o->nb_reordered);
	if (!ret)
		ret = json_array(json_value(text, "duplicated"), o->duplicated, 1, ORACLE_MAX,
				&o->nb_duplicated);

	return ret;
}

static bool in_list(const long *list, unsigned int nb, long val)
{
	unsigned int i;

	for (i = 0; i < nb; i++)
		if (list[i] == val)
			return true;

	return false;
}

static bool reordered_first(const struct oracle *o, long p)
{
	unsigned int i;

	for (i = 0; i < o->nb_reordered; i++)
		if (o->reordered[i][0] == p)
			return true;

	return false;
}

static unsigned int push_emitted(const struct oracle *o, long p, long *order, unsigned int n,
		unsigned int max)
{
	if (n < max)
		order[n++] = p;
	if (n < max && in_list(o->duplicated, o->nb_duplicated, p))
		order[n++] = p;

	return n;
}

/* Packet indices in emission order (vrt_sender --help): dropped indices
 * skipped, a reordered pair [a, a+1] as a+1 then a, a duplicated packet
 * twice in a row */
static unsigned int oracle_order(const struct oracle *o, long *order, unsigned int max)
{
	long p, total = o->sent + (long)o->nb_dropped - (long)o->nb_duplicated;
	unsigned int n = 0;

	for (p = 0; p < total; p++) {
		if (in_list(o->dropped, o->nb_dropped, p))
			continue;

		if (reordered_first(o, p)) {
			n = push_emitted(o, p + 1, order, n, max);
			n = push_emitted(o, p, order, n, max);
			p++;
		} else {
			n = push_emitted(o, p, order, n, max);
		}
	}

	return n;
}

/* The blocks one-packet blocks receive from packets in <order> */
static unsigned int expect_blocks(const long *order, unsigned int nb, bool lossy, long *exp,
		unsigned int max)
{
	long p, g, last = -1;
	unsigned int i, k = 0;

	for (i = 0; i < nb && k < max; i++) {
		p = order[i];
		if (last >= 0 && p <= last)
			continue;

		if (last >= 0 && p > last + 1) {
			if (!lossy)
				exp[k++] = BLK_EIO;
			for (g = p - last - 1; lossy && g && k < max; g--)
				exp[k++] = BLK_ZERO;
		}

		if (k < max)
			exp[k++] = p;
		last = p;
	}

	return k;
}

static void print_blk(const char *what, unsigned int i, long v)
{
	static const char *const names[] = { "-EIO", "zero", "bad", "error" };

	if (v >= 0)
		printf("  block %u %s: packet %ld\n", i, what, v);
	else
		printf("  block %u %s: %s\n", i, what, names[-v - 1]);
}

/* The dequeued blocks are exactly what the oracle predicts */
static void check_blocks(const struct lb_case *c, bool lossy)
{
	static long order[MAX_RECS], exp[MAX_RECS];
	unsigned int i, nb, nb_exp, bad = 0;

	nb = oracle_order(&c->o, order, MAX_RECS);
	nb_exp = expect_blocks(order, nb, lossy, exp, MAX_RECS);

	CHECK_EQ(c->nb_recs, nb_exp, "blocks dequeued, as the oracle predicts");
	for (i = 0; i < c->nb_recs && i < nb_exp; i++) {
		if (c->recs[i] == exp[i])
			continue;
		if (bad++ < 4) {
			print_blk("got", i, c->recs[i]);
			print_blk("expected", i, exp[i]);
		}
	}
	CHECK_EQ(bad, 0, "every block as the oracle predicts");
}

/* Kill and reap the sender, if any */
static void sender_kill(struct lb_case *c)
{
	if (c->sender <= 0)
		return;

	kill(c->sender, SIGKILL);
	while (waitpid(c->sender, NULL, 0) < 0 && errno == EINTR)
		;
	c->sender = -1;
}

/* The sender's exit status; -1 when it was killed by a signal or did not
 * exit within timeout_ms (it is then killed). Reaps it either way. */
static int sender_wait(struct lb_case *c, int timeout_ms)
{
	struct timespec t0;
	int status;
	pid_t ret;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (;;) {
		ret = waitpid(c->sender, &status, WNOHANG);
		if (ret == c->sender)
			break;
		if (ret < 0 && errno != EINTR) {
			sender_kill(c);
			return -1;
		}
		if (ms_since(&t0) > timeout_ms) {
			printf("  vrt_sender still running after %d ms\n", timeout_ms);
			sender_kill(c);
			return -1;
		}
		sleep_ms(1);
	}

	c->sender = -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);

	printf("  vrt_sender killed by signal %d\n", WTERMSIG(status));
	return -1;
}

static void case_close(struct lb_case *c)
{
	sender_kill(c);

	while (c->nb_blocks)
		iio_block_destroy(c->blocks[--c->nb_blocks]);
	if (c->stream)
		iio_buffer_close(c->stream);
	if (c->mask)
		iio_channels_mask_destroy(c->mask);
	if (c->ctx)
		iio_context_destroy(c->ctx);

	c->stream = NULL;
	c->mask = NULL;
	c->ctx = NULL;
}

/* Context with <extra> URI options, buffer with <nb_blocks> blocks */
static int case_open(struct lb_case *c, const char *extra, unsigned int nb_blocks)
{
	struct iio_block *block;
	const struct iio_device *dev;
	struct iio_channel *chn;
	char uri[4300];
	int ret;

	memset(c, 0, sizeof(*c));
	c->sender = -1;

	snprintf(uri, sizeof(uri), "daqiri:127.0.0.1,framing=vrt,yaml=%s%s", yaml, extra);
	c->ctx = iio_create_context(NULL, uri);
	ret = iio_err(c->ctx);
	if (ret) {
		printf("  %s: unable to create context: %d\n", uri, ret);
		c->ctx = NULL;
		return ret;
	}

	ret = iio_context_set_timeout(c->ctx, DEQUEUE_TIMEOUT_MS);
	if (ret)
		return ret;

	dev = iio_context_find_device(c->ctx, DEV_NAME);
	if (!dev)
		return -ENODEV;

	c->mask = iio_create_channels_mask(iio_device_get_channels_count(dev));
	if (!c->mask)
		return -ENOMEM;

	chn = iio_device_find_channel(dev, "voltage0", false);
	if (!chn)
		return -ENOENT;
	iio_channel_enable(chn, c->mask);
	chn = iio_device_find_channel(dev, "voltage1", false);
	if (!chn)
		return -ENOENT;
	iio_channel_enable(chn, c->mask);

	c->buf = iio_device_get_buffer(dev, 0);
	if (!c->buf)
		return -ENODEV;

	c->stream = iio_buffer_open(c->buf, c->mask);
	ret = iio_err(c->stream);
	if (ret) {
		c->stream = NULL;
		return ret;
	}

	while (c->nb_blocks < nb_blocks) {
		block = iio_buffer_stream_create_block(c->stream, BLOCK_SIZE);
		ret = iio_err(block);
		if (ret)
			return ret;
		c->blocks[c->nb_blocks++] = block;
	}

	return 0;
}

/*
 * Open, queue every block, start the stream, then fork vrt_sender with
 * --to DEST --json <json_path> and <args> (NULL-terminated).
 */
static int case_start(struct lb_case *c, const char *extra, unsigned int nb_blocks,
		const char *const *args)
{
	const char *argv[32];
	unsigned int i, n = 0;
	pid_t pid;
	int ret;

	ret = case_open(c, extra, nb_blocks);
	for (i = 0; !ret && i < c->nb_blocks; i++)
		ret = iio_block_enqueue(c->blocks[i], 0, false);
	if (!ret)
		ret = iio_buffer_stream_start(c->stream);
	if (ret)
		return ret;

	argv[n++] = sender_path;
	argv[n++] = "--to";
	argv[n++] = DEST;
	argv[n++] = "--json";
	argv[n++] = json_path;
	for (i = 0; args[i] && n < sizeof(argv) / sizeof(argv[0]) - 1; i++)
		argv[n++] = args[i];
	argv[n] = NULL;

	/* No stale oracle */
	unlink(json_path);

	clock_gettime(CLOCK_MONOTONIC, &c->t_start);

	/* Threads are running: the child only execs */
	pid = fork();
	if (pid < 0)
		return -errno;
	if (!pid) {
		execv(sender_path, (char *const *)argv);
		_exit(127);
	}
	c->sender = pid;

	return 0;
}

/*
 * Dequeue in order until the block holding packet <last>, a dequeue error
 * other than -EIO, or MAX_RECS blocks. Every other block is queued again.
 */
static void case_stream(struct lb_case *c, long last)
{
	struct iio_block *b;
	long rec;
	int ret;

	while (c->nb_recs < MAX_RECS) {
		b = c->blocks[c->next];
		ret = iio_block_dequeue(b, false);
		if (ret == -EIO) {
			rec = BLK_EIO;
		} else if (ret) {
			printf("  dequeue of block %u: %d\n", c->nb_recs, ret);
			rec = BLK_ERR;
		} else {
			rec = classify(b);
		}

		c->recs[c->nb_recs++] = rec;
		c->next = (c->next + 1) % c->nb_blocks;
		if (rec == BLK_ERR || rec == last)
			return;

		ret = iio_block_enqueue(b, 0, false);
		if (ret) {
			printf("  enqueue after block %u: %d\n", c->nb_recs - 1, ret);
			return;
		}
	}
}

/* Datagrams the backend has seen: delivered, late or duplicate */
static long long arrived(const struct lb_case *c)
{
	return attr_ll(c->buf, "daqiri_rx_packets") + attr_ll(c->buf, "daqiri_rx_late_packets") +
	       attr_ll(c->buf, "daqiri_rx_duplicate_packets");
}

/*
 * Wait for the sender to exit 0 and read its oracle, then until the backend
 * has seen every datagram the oracle says was sent. False on any failure.
 */
static bool case_finish(struct lb_case *c)
{
	struct timespec t0;
	int status, ret;

	status = sender_wait(c, SENDER_TIMEOUT_MS);
	CHECK_EQ(status, 0, "vrt_sender exit status");
	if (status)
		return false;

	ret = oracle_read(json_path, &c->o);
	CHECK_EQ(ret, 0, "oracle read");
	if (ret)
		return false;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (arrived(c) != c->o.sent && ms_since(&t0) < SETTLE_TIMEOUT_MS)
		sleep_ms(1);

	CHECK_EQ(arrived(c), c->o.sent, "backend saw every datagram sent");

	return arrived(c) == c->o.sent;
}

/* Nothing completed beyond what was dequeued */
static void check_no_more_blocks(const struct lb_case *c)
{
	CHECK_EQ(iio_block_dequeue(c->blocks[c->next], true), -EBUSY, "no further block completed");
}

static void check_oracle_list(const long *got, unsigned int nb, const long *exp, unsigned int nb_exp,
		const char *message)
{
	bool same = nb == nb_exp;
	unsigned int i;

	for (i = 0; same && i < nb; i++)
		same = got[i] == exp[i];

	TEST_ASSERT(same, message);
}

static long ctr(const struct lb_case *c, const char *name)
{
	return (long)attr_ll(c->buf, name);
}

/* 64 packets, nothing lost: 64 consecutive ramp blocks */
TEST_FUNCTION(clean)
{
	static const char *const args[] = { "--packets", "64", "--pps", "1000", "--source", "ramp",
		NULL };
	struct lb_case *c = &lc;
	unsigned int i, ok = 0;
	int ret;

	ret = case_start(c, "", NB_BLOCKS, args);
	CHECK_EQ(ret, 0, "stream started, sender forked");
	if (ret)
		goto out;

	case_stream(c, 63);
	if (!case_finish(c))
		goto out;

	CHECK_EQ(c->o.sent, 64, "oracle: 64 sent");
	CHECK_EQ(c->o.nb_dropped + c->o.nb_reordered + c->o.nb_duplicated, 0, "oracle: clean");

	CHECK_EQ(c->nb_recs, 64, "64 blocks");
	for (i = 0; i < c->nb_recs; i++)
		ok += c->recs[i] == (long)i;
	CHECK_EQ(ok, 64, "block k is ramp16(k * 360 + n)");
	check_blocks(c, false);
	check_no_more_blocks(c);

	CHECK_EQ(ctr(c, "daqiri_rx_packets"), 64, "rx_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_bytes"), 64 * BLOCK_SIZE, "rx_bytes");
	CHECK_EQ(ctr(c, "daqiri_rx_lost_packets"), 0, "lost_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_gap_events"), 0, "gap_events");
	CHECK_EQ(ctr(c, "daqiri_rx_dropped_no_block"), 0, "dropped_no_block");
	CHECK_EQ(ctr(c, "daqiri_blocks_completed"), 64, "blocks_completed");
	CHECK_EQ(ctr(c, "daqiri_blocks_failed"), 0, "blocks_failed");
	CHECK_EQ(ctr(c, "daqiri_blocks_with_gaps"), 0, "blocks_with_gaps");
out:
	case_close(c);
}

/* Shared by drop_list and drop_list_lossy */
static const char *const drop_args[] = { "--packets", "64", "--pps", "1000", "--drop-list",
	"5,6,20", NULL };
static const long drop_seqs[] = { 5, 6, 20 };

/* lossy=0: one -EIO block for the 5,6 gap, one for 20 */
TEST_FUNCTION(drop_list)
{
	struct lb_case *c = &lc;
	int ret;

	ret = case_start(c, "", NB_BLOCKS, drop_args);
	CHECK_EQ(ret, 0, "stream started, sender forked");
	if (ret)
		goto out;

	case_stream(c, 63);
	if (!case_finish(c))
		goto out;

	CHECK_EQ(c->o.sent, 61, "oracle: 61 sent");
	check_oracle_list(c->o.dropped, c->o.nb_dropped, drop_seqs, 3, "oracle: dropped 5, 6, 20");

	/* 0..4, -EIO, 7..19, -EIO, 21..63 */
	CHECK_EQ(c->nb_recs, 63, "63 blocks");
	if (c->nb_recs == 63) {
		CHECK_EQ(c->recs[4], 4, "block 4: packet 4");
		CHECK_EQ(c->recs[5], BLK_EIO, "block 5: -EIO (packets 5 and 6 lost)");
		CHECK_EQ(c->recs[6], 7, "block 6: packet 7");
		CHECK_EQ(c->recs[18], 19, "block 18: packet 19");
		CHECK_EQ(c->recs[19], BLK_EIO, "block 19: -EIO (packet 20 lost)");
		CHECK_EQ(c->recs[20], 21, "block 20: packet 21");
		CHECK_EQ(c->recs[62], 63, "block 62: packet 63");
	}
	check_blocks(c, false);
	check_no_more_blocks(c);

	CHECK_EQ(ctr(c, "daqiri_rx_packets"), 61, "rx_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_lost_packets"), 3, "lost_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_gap_events"), 2, "gap_events");
	CHECK_EQ(ctr(c, "daqiri_blocks_failed"), 2, "blocks_failed");
	CHECK_EQ(ctr(c, "daqiri_blocks_completed"), 61, "blocks_completed");
	CHECK_EQ(ctr(c, "daqiri_blocks_with_gaps"), 2, "blocks_with_gaps (the failed ones)");
	CHECK_EQ(ctr(c, "daqiri_rx_zero_filled_bytes"), 0, "zero_filled_bytes");
	CHECK_EQ(ctr(c, "daqiri_rx_dropped_no_block"), 0, "dropped_no_block");
out:
	case_close(c);
}

/* lossy=1: three all-zero blocks where 5, 6 and 20 were, stream aligned */
TEST_FUNCTION(drop_list_lossy)
{
	struct lb_case *c = &lc;
	unsigned int i, ok = 0;
	int ret;

	ret = case_start(c, ",lossy=1", NB_BLOCKS, drop_args);
	CHECK_EQ(ret, 0, "stream started (lossy=1), sender forked");
	if (ret)
		goto out;

	case_stream(c, 63);
	if (!case_finish(c))
		goto out;

	CHECK_EQ(c->o.sent, 61, "oracle: 61 sent");
	check_oracle_list(c->o.dropped, c->o.nb_dropped, drop_seqs, 3, "oracle: dropped 5, 6, 20");

	CHECK_EQ(c->nb_recs, 64, "64 blocks, none failed");
	for (i = 0; i < c->nb_recs; i++) {
		if (in_list(drop_seqs, 3, (long)i))
			ok += c->recs[i] == BLK_ZERO;
		else
			ok += c->recs[i] == (long)i;
	}
	CHECK_EQ(ok, 64, "blocks 5, 6, 20 all zero, block k = ramp packet k otherwise");
	check_blocks(c, true);
	check_no_more_blocks(c);

	CHECK_EQ(ctr(c, "daqiri_rx_packets"), 61, "rx_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_lost_packets"), 3, "lost_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_gap_events"), 2, "gap_events");
	CHECK_EQ(ctr(c, "daqiri_rx_zero_filled_bytes"), 3 * BLOCK_SIZE, "zero_filled_bytes");
	CHECK_EQ(ctr(c, "daqiri_blocks_with_gaps"), 3, "blocks_with_gaps");
	CHECK_EQ(ctr(c, "daqiri_blocks_failed"), 0, "blocks_failed");
	CHECK_EQ(ctr(c, "daqiri_blocks_completed"), 64, "blocks_completed");
	CHECK_EQ(ctr(c, "daqiri_rx_dropped_no_block"), 0, "dropped_no_block");
out:
	case_close(c);
}

/* Pairs (3m-1, 3m) swapped: each counts a loss, then un-counts it late */
TEST_FUNCTION(reorder)
{
	static const char *const args[] = { "--packets", "64", "--pps", "1000", "--reorder-window",
		"3", NULL };
	struct lb_case *c = &lc;
	long exp_pairs[21][2], prev = -1;
	unsigned int i, nb_eio = 0, nb_ok = 0;
	bool increasing = true;
	int ret;

	ret = case_start(c, "", NB_BLOCKS, args);
	CHECK_EQ(ret, 0, "stream started, sender forked");
	if (ret)
		goto out;

	case_stream(c, 63);
	if (!case_finish(c))
		goto out;

	/* --reorder-window 3 on 64 packets: [2,3], [5,6], ..., [62,63] */
	for (i = 0; i < 21; i++) {
		exp_pairs[i][0] = 3 * (long)i + 2;
		exp_pairs[i][1] = 3 * (long)i + 3;
	}
	CHECK_EQ(c->o.sent, 64, "oracle: 64 sent");
	CHECK_EQ(c->o.nb_dropped, 0, "oracle: none dropped");
	check_oracle_list(&c->o.reordered[0][0], c->o.nb_reordered * 2, &exp_pairs[0][0], 21 * 2,
			"oracle: 21 pairs [3m+2, 3m+3]");

	for (i = 0; i < c->nb_recs; i++) {
		if (c->recs[i] == BLK_EIO) {
			nb_eio++;
			continue;
		}
		nb_ok++;
		increasing = increasing && c->recs[i] > prev;
		prev = c->recs[i];
	}
	CHECK_EQ(c->nb_recs, 64, "64 blocks");
	CHECK_EQ(nb_eio, c->o.nb_reordered, "one -EIO block per reordered pair");
	CHECK_EQ(nb_ok, 64 - c->o.nb_reordered, "late packets delivered no block");
	TEST_ASSERT(increasing && prev == 63, "valid blocks: whole ramp packets, in order, to 63");
	check_blocks(c, false);
	check_no_more_blocks(c);

	CHECK_EQ(ctr(c, "daqiri_rx_lost_packets"), 0, "lost_packets: late un-counts the loss");
	CHECK_EQ(ctr(c, "daqiri_rx_late_packets"), c->o.nb_reordered, "late_packets == pairs");
	CHECK_EQ(ctr(c, "daqiri_rx_gap_events"), c->o.nb_reordered, "gap_events == pairs");
	CHECK_EQ(ctr(c, "daqiri_blocks_failed"), c->o.nb_reordered, "blocks_failed == pairs");
	CHECK_EQ(ctr(c, "daqiri_rx_packets"), 64 - c->o.nb_reordered, "rx_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_dropped_no_block"), 0, "dropped_no_block");
out:
	case_close(c);
}

/*
 * Two blocks queued, never dequeued while 200 packets go by at 10 kpps.
 * The poller keeps draining: packets 0 and 1 fill the blocks, each later
 * packet finds no block and is counted (dropped_no_block, per packet).
 */
TEST_FUNCTION(slow_consumer)
{
	static const char *const args[] = { "--packets", "200", "--pps", "10000", NULL };
	struct lb_case *c = &lc;
	long packets;
	int ret;

	ret = case_start(c, "", 2, args);
	CHECK_EQ(ret, 0, "stream started with 2 blocks, sender forked");
	if (ret)
		goto out;

	/* No dequeue until the sender is done, every packet was seen and at
	 * least 100 ms went by */
	if (!case_finish(c))
		goto out;
	while (ms_since(&c->t_start) < 100.0)
		sleep_ms(1);

	CHECK_EQ(c->o.sent, 200, "oracle: 200 sent");
	TEST_ASSERT(ms_since(&c->t_start) >= 19.9, "sender took >= 19.9 ms (200 at 10 kpps)");

	CHECK_EQ(iio_block_dequeue(c->blocks[0], false), 0, "block 0 done");
	CHECK_EQ(classify(c->blocks[0]), 0, "block 0 is ramp packet 0");
	CHECK_EQ(iio_block_dequeue(c->blocks[1], false), 0, "block 1 done");
	CHECK_EQ(classify(c->blocks[1]), 1, "block 1 is ramp packet 1");

	packets = ctr(c, "daqiri_rx_packets");
	CHECK_EQ(packets, 200, "rx_packets: loopback lost nothing");
	CHECK_EQ(ctr(c, "daqiri_rx_dropped_no_block"), packets - 2,
			"dropped_no_block: every packet after the 2 blocks");
	CHECK_EQ(ctr(c, "daqiri_rx_dropped_no_block"), 198, "dropped_no_block == 198");
	CHECK_EQ(ctr(c, "daqiri_rx_lost_packets"), 0, "lost_packets");
	CHECK_EQ(ctr(c, "daqiri_blocks_completed"), 2, "blocks_completed");
out:
	case_close(c);
}

/* 32 packets, 500 ms of silence, 32 more: the stream resumes, no loss */
TEST_FUNCTION(burst_gap)
{
	static const char *const args[] = { "--packets", "64", "--pps", "1000", "--burst-gap-ms",
		"500", NULL };
	struct lb_case *c = &lc;
	unsigned int i, ok = 0;
	double elapsed;
	int ret;

	ret = case_start(c, "", NB_BLOCKS, args);
	CHECK_EQ(ret, 0, "stream started, sender forked");
	if (ret)
		goto out;

	case_stream(c, 63);
	/* Packet 63 left the sender at least 500 ms after it started */
	elapsed = ms_since(&c->t_start);
	if (!case_finish(c))
		goto out;

	CHECK_EQ(c->o.sent, 64, "oracle: 64 sent");
	TEST_ASSERT(elapsed >= 500.0, "last block came after the 500 ms gap");

	CHECK_EQ(c->nb_recs, 64, "64 blocks");
	for (i = 0; i < c->nb_recs; i++)
		ok += c->recs[i] == (long)i;
	CHECK_EQ(ok, 64, "block k is ramp packet k across the gap");
	check_blocks(c, false);
	check_no_more_blocks(c);

	CHECK_EQ(ctr(c, "daqiri_rx_packets"), 64, "rx_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_lost_packets"), 0, "lost_packets");
	CHECK_EQ(ctr(c, "daqiri_rx_gap_events"), 0, "gap_events");
	CHECK_EQ(ctr(c, "daqiri_blocks_completed"), 64, "blocks_completed");
	CHECK_EQ(ctr(c, "daqiri_rx_dropped_no_block"), 0, "dropped_no_block");
out:
	case_close(c);
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	int fd;

	setvbuf(stdout, NULL, _IONBF, 0);

	yaml = getenv("DAQIRI_YAML");
	if (!yaml || !yaml[0]) {
		printf("DAQIRI_YAML must be the path of socket-udp-loopback.yaml\n");
		return 1;
	}

	sender_path = getenv("VRT_SENDER");
	if (!sender_path || access(sender_path, X_OK)) {
		printf("VRT_SENDER (%s) must be the vrt_sender executable\n",
				sender_path ? sender_path : "unset");
		return 1;
	}

	snprintf(json_path, sizeof(json_path), "%s/vrt-loopback-XXXXXX",
			tmp && tmp[0] ? tmp : "/tmp");
	fd = mkstemp(json_path);
	if (fd < 0) {
		printf("%s: %s\n", json_path, strerror(errno));
		return 1;
	}
	close(fd);

	RUN_TEST(clean);
	RUN_TEST(drop_list);
	RUN_TEST(drop_list_lossy);
	RUN_TEST(reorder);
	RUN_TEST(slow_consumer);
	RUN_TEST(burst_gap);

	unlink(json_path);

	printf("test_vrt_loopback: %d/%d passed, %d failed\n", test_passed, test_count,
			test_failed);

	return test_failed ? 1 : 0;
}
