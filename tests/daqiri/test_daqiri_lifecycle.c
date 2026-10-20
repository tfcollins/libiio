/* SPDX-License-Identifier: MIT */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Lifecycle test of daqiri:<host>,framing=vrt over the DAQIRI socket engine,
 * against iiod-emu serving fmcomms2.xml, with the engine YAML from the
 * DAQIRI_YAML environment variable (tests/daqiri/resources/
 * socket-udp-loopback.yaml).
 *
 * The engine is process-wide and is never shut down, so the cases share
 * state and run in order: (a) initialises the engine with DAQIRI_YAML, which
 * is what makes a second YAML fail in (d); (a)-(c) use one context, which
 * (c) destroys with its buffer still open; (e)-(h) then need the queue that
 * (c) must have released.
 *
 * (h) goes beyond the brief's (a)-(g): it sends a few hand-built VRT packets
 * from 127.0.0.1:25102 (the YAML's remote_addr; the engine's socket only
 * accepts that source) to check that blocks complete, fail on a gap and fail
 * for good on a layout mismatch. Loss statistics are test_vrt_loopback's.
 *
 * exit_with_open_buffer runs before (a), while this process has no engine:
 * forked children start one each and exit with a buffer streaming.
 *
 * With --mixed (served float_scan_elements.xml), only the channel-length
 * rule is checked: 16- and 32-bit channels in one buffer are -ENOTSUP.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <iio/iio.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_framework.h"

#define DEV_NAME "cf-ad9361-lpc"
#define DDS_NAME "cf-ad9361-dds-core-lpc"

/* voltage0+voltage1, le:S16 each: 4 bytes per sample, one 360-sample packet
 * per block */
#define SPP 360
#define BLOCK_SIZE (SPP * 4)
#define MAX_BLOCKS 4

#define LOCAL_PORT 25101  /* YAML local_addr */
#define SENDER_PORT 25102 /* YAML remote_addr */

#define SID 0x01000000u
#define OUI 0x000456u
#define PCC_CI16 0x0010
#define PCC_REAL_INT16 0x0030

/* CHECK_EQ() evaluates <actual> again to print it: evaluate once */
#define CHECK_EQ(actual, expected, message) \
	do { \
		long check_val_ = (long)(actual); \
		TEST_ASSERT_EQ(check_val_, (long)(expected), message); \
	} while (0)

static const char *yaml;

struct rx {
	struct iio_context *ctx;
	struct iio_buffer *buf;
	struct iio_channels_mask *mask;
	struct iio_buffer_stream *stream;
	struct iio_block *blocks[MAX_BLOCKS];
	unsigned int nb_blocks;
};

/* (a)-(c) share this one */
static struct rx shared;

/* <extra>: more URI options, "" or ",key=value..." */
static struct iio_context *ctx_opts(const char *path, const char *extra)
{
	struct iio_context *ctx;
	char uri[4300];

	snprintf(uri, sizeof(uri), "daqiri:127.0.0.1,framing=vrt,yaml=%s%s", path, extra);
	ctx = iio_create_context(NULL, uri);
	if (iio_err(ctx)) {
		printf("  %s: unable to create context: %d\n", uri, iio_err(ctx));
		return NULL;
	}

	return ctx;
}

static struct iio_context *ctx_for(const char *path)
{
	return ctx_opts(path, "");
}

static void rx_close(struct rx *rx)
{
	while (rx->nb_blocks)
		iio_block_destroy(rx->blocks[--rx->nb_blocks]);
	if (rx->stream)
		iio_buffer_close(rx->stream);
	rx->stream = NULL;
}

static void rx_destroy(struct rx *rx)
{
	rx_close(rx);
	if (rx->mask)
		iio_channels_mask_destroy(rx->mask);
	if (rx->ctx)
		iio_context_destroy(rx->ctx);
	memset(rx, 0, sizeof(*rx));
}

static const char *const iq_chans[] = { "voltage0", "voltage1" };

/* Mask of chans[0]+chans[1] of <dev_name> (outputs if <output>), buffer 0. */
static int rx_init_chans(struct rx *rx, struct iio_context *ctx, const char *dev_name,
		const char *const *chans, bool output)
{
	const struct iio_device *dev;
	struct iio_channel *chn;
	unsigned int i;

	memset(rx, 0, sizeof(*rx));
	rx->ctx = ctx;
	if (!ctx)
		return -ENODEV;

	dev = iio_context_find_device(ctx, dev_name);
	if (!dev)
		return -ENODEV;

	rx->mask = iio_create_channels_mask(iio_device_get_channels_count(dev));
	if (!rx->mask)
		return -ENOMEM;

	for (i = 0; i < 2; i++) {
		chn = iio_device_find_channel(dev, chans[i], output);
		if (!chn)
			return -ENOENT;
		iio_channel_enable(chn, rx->mask);
	}

	rx->buf = iio_device_get_buffer(dev, 0);

	return rx->buf ? 0 : -ENODEV;
}

/* voltage0+voltage1 of <dev_name> */
static int rx_init(struct rx *rx, struct iio_context *ctx, const char *dev_name, bool output)
{
	return rx_init_chans(rx, ctx, dev_name, iq_chans, output);
}

static int rx_open(struct rx *rx, unsigned int nb_blocks)
{
	struct iio_block *block;
	int ret;

	rx->stream = iio_buffer_open(rx->buf, rx->mask);
	ret = iio_err(rx->stream);
	if (ret) {
		rx->stream = NULL;
		return ret;
	}

	while (rx->nb_blocks < nb_blocks) {
		block = iio_buffer_stream_create_block(rx->stream, BLOCK_SIZE);
		ret = iio_err(block);
		if (ret)
			return ret;
		rx->blocks[rx->nb_blocks++] = block;
	}

	return 0;
}

static int enqueue_all(struct rx *rx)
{
	unsigned int i;
	int ret;

	for (i = 0; i < rx->nb_blocks; i++) {
		ret = iio_block_enqueue(rx->blocks[i], 0, false);
		if (ret)
			return ret;
	}

	return 0;
}

static const char *attr_str(const struct iio_buffer *buf, const char *name)
{
	static char val[64];
	const struct iio_attr *attr = iio_buffer_find_attr(buf, name);
	ssize_t ret;

	if (!attr)
		return "(missing)";

	ret = iio_attr_read_raw(attr, val, sizeof(val));
	if (ret < 0)
		snprintf(val, sizeof(val), "(error %zd)", ret);

	return val;
}

static long long attr_ll(const struct iio_buffer *buf, const char *name)
{
	const struct iio_attr *attr = iio_buffer_find_attr(buf, name);
	long long val;

	if (!attr || iio_attr_read_longlong(attr, &val))
		return -1;

	return val;
}

static ssize_t attr_write(const struct iio_buffer *buf, const char *name, const char *val)
{
	const struct iio_attr *attr = iio_buffer_find_attr(buf, name);

	return attr ? iio_attr_write_string(attr, val) : -ENOENT;
}

static bool module_mapped(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	bool found = false;
	char line[4096];

	if (!f)
		return false;

	while (!found && fgets(line, sizeof(line), f))
		found = strstr(line, "/libiio-daqiri.so") != NULL;
	fclose(f);

	return found;
}

/* Exit (not destroy) with a buffer streaming; the child must exit cleanly */
static void exit_child(unsigned int sleep_ms)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)sleep_ms * 1000000L };
	struct rx rx;

	if (rx_init(&rx, ctx_for(yaml), DEV_NAME, false) || rx_open(&rx, 2) || enqueue_all(&rx) ||
			iio_buffer_stream_start(rx.stream))
		_exit(100);

	nanosleep(&ts, NULL);
	exit(0);
}

/* The process has no engine yet: each child starts its own */
TEST_FUNCTION(exit_with_open_buffer)
{
	unsigned int i, clean = 0;
	int status;
	pid_t pid;

	for (i = 0; i < 20; i++) {
		pid = fork();
		if (pid < 0)
			break;
		if (!pid)
			exit_child(20 + (i % 4) * 15);

		if (waitpid(pid, &status, 0) != pid)
			continue;
		if (WIFEXITED(status) && !WEXITSTATUS(status))
			clean++;
		else if (WIFSIGNALED(status))
			printf("  child %u: signal %d\n", i, WTERMSIG(status));
		else
			printf("  child %u: exit status %d\n", i, WEXITSTATUS(status));
	}

	CHECK_EQ(clean, 20, "20 children exited with status 0 while streaming");
}

/* (a) blocks queued, nothing sent: nonblocking dequeue is -EBUSY */
TEST_FUNCTION(busy_when_empty)
{
	struct rx *rx = &shared;
	int ret;

	ret = rx_init(rx, ctx_for(yaml), DEV_NAME, false);
	CHECK_EQ(ret, 0, "context and mask");
	if (ret)
		return;

	ret = rx_open(rx, 2);
	CHECK_EQ(ret, 0, "framing=vrt buffer opens with 2 blocks of 1440");
	if (ret)
		return;

	CHECK_EQ(iio_block_dequeue(rx->blocks[0], false), -EPERM,
			"dequeue of a block never enqueued");
	CHECK_EQ(iio_block_enqueue(rx->blocks[0], BLOCK_SIZE / 2, false), -EINVAL,
			"partial RX block rejected");
	CHECK_EQ(iio_block_enqueue(rx->blocks[0], 0, true), -ENOTSUP, "cyclic RX block rejected");

	CHECK_EQ(enqueue_all(rx), 0, "enqueue both blocks");
	CHECK_EQ(iio_block_enqueue(rx->blocks[0], 0, false), -EPERM, "double enqueue");
	CHECK_EQ(iio_buffer_stream_start(rx->stream), 0, "enable");

	CHECK_EQ(iio_block_dequeue(rx->blocks[0], true), -EBUSY, "nonblocking dequeue");

	/* The block stays queued after a timeout */
	CHECK_EQ(iio_context_set_timeout(rx->ctx, 100), 0, "timeout 100 ms");
	CHECK_EQ(iio_block_dequeue(rx->blocks[0], false), -ETIMEDOUT, "blocking dequeue times out");
	CHECK_EQ(iio_block_dequeue(rx->blocks[0], true), -EBUSY, "still queued");
	CHECK_EQ(iio_context_set_timeout(rx->ctx, 0), 0, "default timeout");
}

/* (b) a cancelled buffer fails blocking dequeues with -ECANCELED */
TEST_FUNCTION(cancel_fails_dequeue)
{
	struct rx *rx = &shared;

	if (!rx->stream || rx->nb_blocks != 2) {
		TEST_ASSERT(false, "needs the buffer from (a)");
		return;
	}

	iio_buffer_stream_cancel(rx->stream);
	CHECK_EQ(iio_block_dequeue(rx->blocks[0], false), -ECANCELED, "block 0 cancelled");
	CHECK_EQ(iio_block_dequeue(rx->blocks[1], false), -ECANCELED, "block 1 cancelled");
	CHECK_EQ(iio_block_dequeue(rx->blocks[0], false), -EPERM, "block 0 now free");
	CHECK_EQ(iio_block_enqueue(rx->blocks[0], 0, false), -ECANCELED, "no enqueue after cancel");
}

/* (c) context destroyed with the buffer and its blocks still open */
TEST_FUNCTION(destroy_with_open_buffer)
{
	struct rx *rx = &shared;

	if (!rx->ctx) {
		TEST_ASSERT(false, "needs the context from (a)");
		return;
	}

	/* The stream and the blocks are leaked on purpose: after this, only
	 * the backend can still reach them. */
	iio_context_destroy(rx->ctx);
	iio_channels_mask_destroy(rx->mask);
	memset(rx, 0, sizeof(*rx));
	TEST_ASSERT(true, "iio_context_destroy returned with a VRT buffer open");

	/* That was the last context, so the core dlclose()d the module: the
	 * engine's threads still run, it must have stayed loaded */
	TEST_ASSERT(module_mapped(), "libiio-daqiri.so still mapped (pinned)");
}

/* (d) a second YAML, even with the same content, cannot get the engine */
TEST_FUNCTION(second_yaml_busy)
{
	const char *tmp = getenv("TMPDIR");
	char path[4096], data[8192];
	struct rx rx;
	size_t len;
	FILE *in, *out;
	int fd, ret;

	snprintf(path, sizeof(path), "%s/daqiri-lifecycle-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
	fd = mkstemp(path);
	TEST_ASSERT(fd >= 0, "temporary YAML copy created");
	if (fd < 0)
		return;

	in = fopen(yaml, "rb");
	out = fdopen(fd, "wb");
	len = in ? fread(data, 1, sizeof(data), in) : 0;
	TEST_ASSERT(in && out && len > 0 && len < sizeof(data) && fwrite(data, 1, len, out) == len,
			"YAML copied");
	if (in)
		fclose(in);
	if (out)
		fclose(out);
	else
		close(fd);

	ret = rx_init(&rx, ctx_for(path), DEV_NAME, false);
	CHECK_EQ(ret, 0, "context with the copy is created");
	if (!ret)
		CHECK_EQ(rx_open(&rx, 0), -EBUSY, "buffer open with another YAML");

	rx_destroy(&rx);
	unlink(path);
}

/* (e) one (port, queue) per buffer */
TEST_FUNCTION(same_queue_busy)
{
	struct iio_buffer_stream *second;
	struct rx rx;
	int ret;

	ret = rx_init(&rx, ctx_for(yaml), DEV_NAME, false);
	CHECK_EQ(ret, 0, "context and mask");
	if (ret)
		goto out;

	ret = rx_open(&rx, 0);
	CHECK_EQ(ret, 0, "queue released by (c), buffer opens");
	if (ret)
		goto out;

	second = iio_buffer_open(rx.buf, rx.mask);
	CHECK_EQ(iio_err(second), -EBUSY, "second open of port 0 queue 0");
	if (!iio_err(second))
		iio_buffer_close(second);

	rx_close(&rx);
	ret = rx_open(&rx, 0);
	CHECK_EQ(ret, 0, "queue released by close, buffer opens again");
out:
	rx_destroy(&rx);
}

/* (f) VRT is RX only */
TEST_FUNCTION(output_rejected)
{
	struct rx rx;
	int ret;

	ret = rx_init(&rx, ctx_for(yaml), DDS_NAME, true);
	CHECK_EQ(ret, 0, "context and DDS mask");
	if (!ret)
		CHECK_EQ(rx_open(&rx, 0), -ENOTSUP, "output buffer");
	rx_destroy(&rx);
}

/* (g) the counters are buffer attributes, served by the backend */
TEST_FUNCTION(counter_attrs)
{
	static const char *const names[] = {
		"daqiri_rx_packets",
		"daqiri_rx_bytes",
		"daqiri_rx_lost_packets",
		"daqiri_rx_gap_events",
		"daqiri_rx_dropped_no_block",
		"daqiri_rx_overrun_events",
		"daqiri_rx_foreign_packets",
		"daqiri_rx_context_packets",
		"daqiri_rx_malformed",
		"daqiri_rx_zero_filled_bytes",
		"daqiri_rx_late_packets",
		"daqiri_rx_duplicate_packets",
		"daqiri_blocks_completed",
		"daqiri_blocks_failed",
		"daqiri_blocks_with_gaps",
		"daqiri_stream_id",
		"daqiri_last_error",
		"daqiri_lossy",
	};
	const struct iio_attr *attr;
	unsigned int i;
	char msg[128];
	struct rx rx;
	int ret;

	ret = rx_init(&rx, ctx_for(yaml), DEV_NAME, false);
	CHECK_EQ(ret, 0, "context and mask");
	if (ret)
		goto out;

	attr = iio_buffer_find_attr(rx.buf, "daqiri_rx_packets");
	TEST_ASSERT_PTR_NOT_NULL(attr, "daqiri_rx_packets exists");
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_rx_packets"), "0", "reads 0 with no stream");
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_mem_domain"), "host", "mem domain, no stream");
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_engine_kind"), "none", "engine, no stream");
	CHECK_EQ(attr_write(rx.buf, "daqiri_lossy", "1"), -EBADF,
			"daqiri_lossy needs an open stream");

	ret = rx_open(&rx, 0);
	CHECK_EQ(ret, 0, "buffer opens");
	if (ret)
		goto out;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(msg, sizeof(msg), "%s reads 0 on a new stream", names[i]);
		TEST_ASSERT_STR_EQ(attr_str(rx.buf, names[i]), "0", msg);
	}
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_mem_domain"), "host", "mem domain");
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_engine_kind"), "socket", "engine kind");

	TEST_ASSERT(attr_write(rx.buf, "daqiri_lossy", "1") > 0, "daqiri_lossy written");
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_lossy"), "1", "daqiri_lossy reads back");
	CHECK_EQ(attr_write(rx.buf, "daqiri_lossy", "2"), -EINVAL, "daqiri_lossy is 0 or 1");
	CHECK_EQ(attr_write(rx.buf, "daqiri_rx_packets", "5"), -EPERM, "counters are read-only");
out:
	rx_destroy(&rx);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

/* I of sample n of the stream; Q is its complement */
static uint16_t sample_i(unsigned int n)
{
	return (uint16_t)(0x1000 + n);
}

/*
 * IF data with SID, Class ID, TSI UTC, TSF sample count = first sample index,
 * SPP CI16 samples (I in the high half of each big-endian word). Returns
 * 0 or -errno.
 */
static int send_pkt(int fd, unsigned int count, unsigned int first, unsigned int pclass)
{
	uint8_t pkt[28 + SPP * 4];
	struct sockaddr_in to = {
		.sin_family = AF_INET,
		.sin_port = htons(LOCAL_PORT),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	unsigned int n;

	put32(pkt, (1u << 28) | (1u << 27) | (1u << 22) | (1u << 20) | ((count & 0xF) << 16) |
					(uint32_t)(sizeof(pkt) / 4));
	put32(pkt + 4, SID);
	put32(pkt + 8, OUI);
	put32(pkt + 12, pclass);
	put32(pkt + 16, 1700000000u);
	put32(pkt + 20, 0);
	put32(pkt + 24, first);
	for (n = 0; n < SPP; n++)
		put32(pkt + 28 + 4 * n, ((uint32_t)sample_i(first + n) << 16) |
							(uint16_t)~sample_i(first + n));

	if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) != sizeof(pkt))
		return -errno;

	return 0;
}

/* After swap16, sample n of the block is I then Q, little-endian S16 */
static bool block_is(const struct iio_block *block, unsigned int first)
{
	const uint8_t *p = iio_block_start(block);
	uint16_t i, q;
	unsigned int n;

	for (n = 0; n < SPP; n++) {
		memcpy(&i, p + 4 * n, 2);
		memcpy(&q, p + 4 * n + 2, 2);
		if (i != sample_i(first + n) || q != (uint16_t)~sample_i(first + n)) {
			printf("  sample %u: I 0x%04x Q 0x%04x\n", n, i, q);
			return false;
		}
	}

	return true;
}

static int sender_open(void)
{
	struct sockaddr_in from = {
		.sin_family = AF_INET,
		.sin_port = htons(SENDER_PORT),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	int fd, one = 1;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -errno;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr *)&from, sizeof(from))) {
		close(fd);
		return -errno;
	}

	return fd;
}

/* (h) packets in: DONE, -EIO on a gap, -EPROTO until disabled */
TEST_FUNCTION(data_smoke)
{
	struct iio_block **b;
	struct rx rx;
	int fd, ret;

	fd = sender_open();
	TEST_ASSERT(fd >= 0, "sender bound to 127.0.0.1:25102");
	if (fd < 0)
		return;

	ret = rx_init(&rx, ctx_for(yaml), DEV_NAME, false);
	if (!ret)
		ret = rx_open(&rx, 4);
	CHECK_EQ(ret, 0, "buffer with 4 blocks");
	if (ret)
		goto out;
	b = rx.blocks;

	CHECK_EQ(enqueue_all(&rx), 0, "enqueue 4");
	CHECK_EQ(iio_buffer_stream_start(rx.stream), 0, "enable");

	/* Two consecutive packets fill two blocks */
	CHECK_EQ(send_pkt(fd, 0, 0, PCC_CI16), 0, "send count 0");
	CHECK_EQ(send_pkt(fd, 1, SPP, PCC_CI16), 0, "send count 1");
	CHECK_EQ(iio_block_dequeue(b[0], false), 0, "block 0 done");
	TEST_ASSERT(block_is(b[0], 0), "block 0 is packet 0, swapped");
	CHECK_EQ(iio_block_dequeue(b[1], false), 0, "block 1 done");
	TEST_ASSERT(block_is(b[1], SPP), "block 1 is packet 1, swapped");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_packets"), 2, "2 packets");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_bytes"), 2 * BLOCK_SIZE, "2 payloads");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_completed"), 2, "2 blocks completed");
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_stream_id"), "0x01000000", "stream id");

	/* Count 2 never arrives: block 2 fails, count 3 lands in block 3 */
	CHECK_EQ(send_pkt(fd, 3, 3 * SPP, PCC_CI16), 0, "send count 3");
	CHECK_EQ(iio_block_dequeue(b[2], false), -EIO, "block 2 failed on the gap");
	CHECK_EQ(iio_block_dequeue(b[3], false), 0, "block 3 done");
	TEST_ASSERT(block_is(b[3], 3 * SPP), "block 3 is packet 3");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_lost_packets"), 1, "1 lost");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_gap_events"), 1, "1 gap event");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_failed"), 1, "1 block failed");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_with_gaps"), 1, "1 block with a gap");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_last_error"), -EIO, "last error");

	/* lossy=1, one-packet blocks: a sized gap past the 1024-packet fill
	 * cap is a resync even right after a block completed. The next block
	 * fails, the packet starts the one after, no zeros. */
	TEST_ASSERT(attr_write(rx.buf, "daqiri_lossy", "1") > 0, "lossy on");
	CHECK_EQ(iio_block_enqueue(b[0], 0, false), 0, "re-enqueue block 0");
	CHECK_EQ(iio_block_enqueue(b[1], 0, false), 0, "re-enqueue block 1");
	CHECK_EQ(iio_block_enqueue(b[2], 0, false), 0, "re-enqueue block 2");
	CHECK_EQ(send_pkt(fd, 4, 4 * SPP, PCC_CI16), 0, "send count 4");
	CHECK_EQ(iio_block_dequeue(b[0], false), 0, "block 0 done, no block current");
	CHECK_EQ(send_pkt(fd, (4 + 2000) & 0xF, (4 + 2000) * SPP, PCC_CI16), 0,
			"send the packet 2000 after count 4");
	CHECK_EQ(iio_block_dequeue(b[1], false), -EIO, "block 1 fails: resync");
	CHECK_EQ(iio_block_dequeue(b[2], false), 0, "block 2 done");
	TEST_ASSERT(block_is(b[2], (4 + 2000) * SPP), "block 2 is the packet after the gap");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_lost_packets"), 1 + 1999, "1999 more lost");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_zero_filled_bytes"), 0, "nothing zero-filled");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_failed"), 2, "2 blocks failed");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_with_gaps"), 2, "2 blocks with a gap");

	rx_close(&rx);

	/* REAL_INT16 class on a 4-byte sample layout: stream-fatal */
	ret = rx_open(&rx, 2);
	CHECK_EQ(ret, 0, "reopened with 2 blocks");
	if (ret)
		goto out;

	CHECK_EQ(enqueue_all(&rx), 0, "enqueue 2");
	CHECK_EQ(iio_buffer_stream_start(rx.stream), 0, "enable");
	CHECK_EQ(send_pkt(fd, 0, 0, PCC_REAL_INT16), 0, "send a REAL_INT16 packet");
	CHECK_EQ(iio_block_dequeue(b[0], false), -EPROTO, "block 0 -EPROTO");
	CHECK_EQ(iio_block_dequeue(b[1], false), -EPROTO, "block 1 -EPROTO too");
	CHECK_EQ(iio_block_enqueue(b[0], 0, false), 0, "enqueue after -EPROTO");
	CHECK_EQ(iio_block_dequeue(b[0], true), -EPROTO, "still -EPROTO");

	/* Disabling clears it */
	CHECK_EQ(iio_buffer_stream_stop(rx.stream), 0, "disable");
	CHECK_EQ(iio_block_enqueue(b[0], 0, false), 0, "enqueue while disabled");
	CHECK_EQ(iio_buffer_stream_start(rx.stream), 0, "enable again");
	CHECK_EQ(send_pkt(fd, 5, 0, PCC_CI16), 0, "send a CI16 packet");
	CHECK_EQ(iio_block_dequeue(b[0], false), 0, "block 0 done after re-enable");
	TEST_ASSERT(block_is(b[0], 0), "block 0 is the CI16 packet");
out:
	rx_destroy(&rx);
	close(fd);
}

/* --mixed, against float_scan_elements.xml: float-dev voltage1 is le:F16/16,
 * voltage2 le:f32/32, voltage5 le:q16/16 */
TEST_FUNCTION(mixed_lengths)
{
	static const char *const mixed[] = { "voltage1", "voltage2" };
	static const char *const same[] = { "voltage1", "voltage5" };
	struct rx rx;
	int ret;

	ret = rx_init_chans(&rx, ctx_opts(yaml, ",swap=0"), "float-dev", mixed, false);
	CHECK_EQ(ret, 0, "swap=0: 16 + 32-bit mask");
	if (!ret)
		CHECK_EQ(rx_open(&rx, 0), -ENOTSUP, "swap=0: mixed lengths rejected");
	rx_destroy(&rx);

	ret = rx_init_chans(&rx, ctx_for(yaml), "float-dev", mixed, false);
	CHECK_EQ(ret, 0, "swap=1: 16 + 32-bit mask");
	if (!ret)
		CHECK_EQ(rx_open(&rx, 0), -ENOTSUP, "swap=1: mixed lengths rejected");
	rx_destroy(&rx);

	/* Control: the same device with two 16-bit channels opens */
	ret = rx_init_chans(&rx, ctx_opts(yaml, ",swap=0"), "float-dev", same, false);
	CHECK_EQ(ret, 0, "16 + 16-bit mask");
	if (!ret)
		CHECK_EQ(rx_open(&rx, 0), 0, "one length opens");
	rx_destroy(&rx);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	yaml = getenv("DAQIRI_YAML");
	if (!yaml || !yaml[0]) {
		printf("DAQIRI_YAML must be the path of socket-udp-loopback.yaml\n");
		return 1;
	}

	if (argc > 1 && !strcmp(argv[1], "--mixed")) {
		RUN_TEST(mixed_lengths);
		goto out;
	}

	RUN_TEST(exit_with_open_buffer);
	RUN_TEST(busy_when_empty);
	RUN_TEST(cancel_fails_dequeue);
	RUN_TEST(destroy_with_open_buffer);
	RUN_TEST(second_yaml_busy);
	RUN_TEST(same_queue_busy);
	RUN_TEST(output_rejected);
	RUN_TEST(counter_attrs);
	RUN_TEST(data_smoke);
out:
	printf("test_daqiri_lifecycle: %d/%d passed, %d failed\n", test_passed, test_count,
			test_failed);

	return test_failed ? 1 : 0;
}
