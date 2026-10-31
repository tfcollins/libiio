/* SPDX-License-Identifier: MIT */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Lifecycle test of daqiri:<host>,data=udp over the DAQIRI socket engine,
 * against iiod-emu serving fmcomms2.xml with --udp-mock-provider, with the
 * engine YAML from the DAQIRI_YAML environment variable (tests/daqiri/
 * resources/socket-udp-loopback.yaml). The mock advertises UDP sample mode,
 * answers every setup with stream ID 0x12345678, max_payload rounded down
 * to 16 bytes and the source 127.0.0.1:25102 (the YAML's remote_addr), and
 * never sends a datagram itself.
 *
 * The engine is process-wide and is never shut down, so the cases share
 * state and run in order: (a) initialises the engine with DAQIRI_YAML, which
 * is what makes a second YAML fail in (d); (a)-(c) use one context, which
 * (c) destroys with its buffer still open; (e)-(h) then need the queue that
 * (c) must have released.
 *
 * (h) sends hand-built datagrams of the mock's stream from 127.0.0.1:25102
 * (the engine's socket only accepts that source) to check that blocks
 * complete with the payloads unchanged, fail on a gap, resync on a gap too
 * large to fill, zero-fill with lossy=1, and that the sequence restarts at 0
 * after a new enable. Loss statistics against a real sender are the UDP
 * loopback tests'.
 *
 * exit_with_open_buffer runs before (a), while this process has no engine:
 * forked children start one each and exit with a buffer streaming.
 *
 * With --mixed (served float_scan_elements.xml), only the channel layout is
 * checked: UDP sample mode takes the device's own layout, so 16- and 32-bit
 * channels open in one buffer.
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

#include "../../iiod-udp-stream.h"
#include "test_framework.h"

#define DEV_NAME "cf-ad9361-lpc"
#define DDS_NAME "cf-ad9361-dds-core-lpc"

/* voltage0+voltage1, le:S16 each: 4 bytes per sample */
#define BLOCK_SIZE (360 * 4)
#define MAX_BLOCKS 4

#define LOCAL_PORT 25101  /* YAML local_addr */
#define SENDER_PORT 25102 /* YAML remote_addr */

/* What the mock answers (iiod-emu/udp-mock.c): the backend asks for 9200
 * bytes, the YAML's 9216-byte buffers less the datagram header */
#define MOCK_STREAM_ID 0x12345678u
#define PAYLOAD 9200
#define SPD (PAYLOAD / 4) /* samples per datagram */

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

	snprintf(uri, sizeof(uri), "daqiri:127.0.0.1,data=udp,yaml=%s%s", path, extra);
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

static int rx_open_size(struct rx *rx, unsigned int nb_blocks, size_t size)
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
		block = iio_buffer_stream_create_block(rx->stream, size);
		ret = iio_err(block);
		if (ret)
			return ret;
		rx->blocks[rx->nb_blocks++] = block;
	}

	return 0;
}

static int rx_open(struct rx *rx, unsigned int nb_blocks)
{
	return rx_open_size(rx, nb_blocks, BLOCK_SIZE);
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
	CHECK_EQ(ret, 0, "data=udp buffer opens with 2 blocks of 1440");
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
	TEST_ASSERT(true, "iio_context_destroy returned with a UDP buffer open");

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

/* (f) UDP sample mode is RX only */
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
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_data_path"), "udp", "data path, no stream");
	TEST_ASSERT_PTR_NULL(iio_buffer_find_attr(rx.buf, "daqiri_rx_context_packets"),
			"no daqiri_rx_context_packets");
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
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_data_path"), "udp", "data path");

	TEST_ASSERT(attr_write(rx.buf, "daqiri_lossy", "1") > 0, "daqiri_lossy written");
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_lossy"), "1", "daqiri_lossy reads back");
	CHECK_EQ(attr_write(rx.buf, "daqiri_lossy", "2"), -EINVAL, "daqiri_lossy is 0 or 1");
	CHECK_EQ(attr_write(rx.buf, "daqiri_rx_packets", "5"), -EPERM, "counters are read-only");
out:
	rx_destroy(&rx);
}

/* Sample n of the stream, as ramp16_le (gen_golden.py): little-endian
 * int16 I = n & 0x7FFF, Q = (0x8000 - n) & 0xFFFF; datagram s carries samples
 * s * SPD to s * SPD + SPD - 1 */
static void put_sample(uint8_t *p, uint32_t n)
{
	uint16_t i = (uint16_t)(n & 0x7FFF), q = (uint16_t)((0x8000 - n) & 0xFFFF);

	p[0] = (uint8_t)i;
	p[1] = (uint8_t)(i >> 8);
	p[2] = (uint8_t)q;
	p[3] = (uint8_t)(q >> 8);
}

/* One datagram of <stream_id> and <version> with sequence number <seq> */
static int send_dgram(int fd, uint32_t stream_id, uint8_t version, uint64_t seq)
{
	static uint8_t dgram[IIOD_UDP_STREAM_HDR_LEN + PAYLOAD];
	const struct iiod_udp_stream_hdr hdr = {
		.version = version,
		.payload_length = PAYLOAD,
		.stream_id = stream_id,
		.sequence = seq,
	};
	struct sockaddr_in to = {
		.sin_family = AF_INET,
		.sin_port = htons(LOCAL_PORT),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	unsigned int n;

	iiod_udp_stream_hdr_pack(&hdr, dgram);
	for (n = 0; n < SPD; n++)
		put_sample(dgram + IIOD_UDP_STREAM_HDR_LEN + 4 * n, (uint32_t)(seq * SPD + n));

	if (sendto(fd, dgram, sizeof(dgram), 0, (struct sockaddr *)&to, sizeof(to)) !=
			sizeof(dgram))
		return -errno;

	return 0;
}

static int send_seq(int fd, uint64_t seq)
{
	return send_dgram(fd, MOCK_STREAM_ID, IIOD_UDP_STREAM_VERSION, seq);
}

/* The payload of datagram <seq>, unchanged: no byte swap */
static bool block_is(const struct iio_block *block, uint64_t seq)
{
	const uint8_t *p = iio_block_start(block);
	uint8_t want[4];
	unsigned int n;

	for (n = 0; n < SPD; n++) {
		put_sample(want, (uint32_t)(seq * SPD + n));
		if (memcmp(p + 4 * n, want, 4)) {
			printf("  sample %u: %02x %02x %02x %02x\n", n, p[4 * n], p[4 * n + 1],
					p[4 * n + 2], p[4 * n + 3]);
			return false;
		}
	}

	return true;
}

static bool block_is_zero(const struct iio_block *block)
{
	const uint8_t *p = iio_block_start(block);
	unsigned int n;

	for (n = 0; n < PAYLOAD; n++)
		if (p[n])
			return false;

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

/* The mock picks payloads of max_payload rounded down to 16 bytes: 9200 is
 * no whole number of 6-byte samples */
TEST_FUNCTION(payload_not_whole_samples)
{
	static const char *const three[] = { "voltage0", "voltage1", "voltage2" };
	struct iio_channel *chn;
	struct rx rx;
	int ret;

	ret = rx_init(&rx, ctx_for(yaml), DEV_NAME, false);
	CHECK_EQ(ret, 0, "context and mask");
	if (ret)
		goto out;

	chn = iio_device_find_channel(iio_buffer_get_device(rx.buf), three[2], false);
	TEST_ASSERT_PTR_NOT_NULL(chn, "voltage2");
	if (!chn)
		goto out;
	iio_channel_enable(chn, rx.mask);

	CHECK_EQ(rx_open(&rx, 0), -EPROTO, "a payload of no whole number of samples");
	CHECK_EQ(rx_open(&rx, 0), -EPROTO, "and again: the queue was given back");
out:
	rx_destroy(&rx);
}

/* (h) datagrams in: DONE, -EIO on a gap, a resync, zero-fill, a restart */
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
		ret = rx_open_size(&rx, 4, PAYLOAD);
	CHECK_EQ(ret, 0, "buffer with 4 blocks of one payload");
	if (ret)
		goto out;
	b = rx.blocks;

	CHECK_EQ(enqueue_all(&rx), 0, "enqueue 4");
	CHECK_EQ(iio_buffer_stream_start(rx.stream), 0, "enable");
	TEST_ASSERT_STR_EQ(attr_str(rx.buf, "daqiri_stream_id"), "0x12345678",
			"stream ID from the setup");

	/* Two consecutive datagrams fill two blocks */
	CHECK_EQ(send_seq(fd, 0), 0, "send 0");
	CHECK_EQ(send_seq(fd, 1), 0, "send 1");
	CHECK_EQ(iio_block_dequeue(b[0], false), 0, "block 0 done");
	TEST_ASSERT(block_is(b[0], 0), "block 0 is datagram 0, unchanged");
	CHECK_EQ(iio_block_dequeue(b[1], false), 0, "block 1 done");
	TEST_ASSERT(block_is(b[1], 1), "block 1 is datagram 1, unchanged");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_packets"), 2, "2 datagrams");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_bytes"), 2 * PAYLOAD, "2 payloads");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_completed"), 2, "2 blocks completed");

	/* 2 never arrives: block 2 fails, 3 lands in block 3 */
	CHECK_EQ(send_seq(fd, 3), 0, "send 3");
	CHECK_EQ(iio_block_dequeue(b[2], false), -EIO, "block 2 failed on the gap");
	CHECK_EQ(iio_block_dequeue(b[3], false), 0, "block 3 done");
	TEST_ASSERT(block_is(b[3], 3), "block 3 is datagram 3");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_lost_packets"), 1, "1 lost");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_gap_events"), 1, "1 gap event");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_failed"), 1, "1 block failed");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_with_gaps"), 1, "1 block with a gap");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_last_error"), -EIO, "last error");

	/* lossy=1, one-datagram blocks: a gap past the 1024-datagram fill cap
	 * is a resync even right after a block completed. The next block
	 * fails, the datagram starts the one after, no zeros. */
	TEST_ASSERT(attr_write(rx.buf, "daqiri_lossy", "1") > 0, "lossy on");
	CHECK_EQ(iio_block_enqueue(b[0], 0, false), 0, "re-enqueue block 0");
	CHECK_EQ(iio_block_enqueue(b[1], 0, false), 0, "re-enqueue block 1");
	CHECK_EQ(iio_block_enqueue(b[2], 0, false), 0, "re-enqueue block 2");
	CHECK_EQ(send_seq(fd, 4), 0, "send 4");
	CHECK_EQ(iio_block_dequeue(b[0], false), 0, "block 0 done, no block current");
	CHECK_EQ(send_seq(fd, 4 + 2000), 0, "send the datagram 2000 after 4");
	CHECK_EQ(iio_block_dequeue(b[1], false), -EIO, "block 1 fails: resync");
	CHECK_EQ(iio_block_dequeue(b[2], false), 0, "block 2 done");
	TEST_ASSERT(block_is(b[2], 4 + 2000), "block 2 is the datagram after the gap");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_lost_packets"), 1 + 1999, "1999 more lost");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_zero_filled_bytes"), 0, "nothing zero-filled");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_failed"), 2, "2 blocks failed");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_blocks_with_gaps"), 2, "2 blocks with a gap");

	/* Dropped and counted, no block touched: another stream, another
	 * version, the last one again, one that comes too late */
	CHECK_EQ(send_dgram(fd, MOCK_STREAM_ID + 1, IIOD_UDP_STREAM_VERSION, 2005), 0,
			"send another stream ID");
	CHECK_EQ(send_dgram(fd, MOCK_STREAM_ID, 2, 2005), 0, "send version 2");
	CHECK_EQ(send_seq(fd, 2004), 0, "send 2004 again");
	CHECK_EQ(send_seq(fd, 2003), 0, "send 2003, late");

	/* lossy=1, a gap of 1: one payload of zeros, then the datagram. The
	 * zeros fill block 0, so datagram 2006 waits in the carry for the next
	 * datagram, which attaches block 1. */
	CHECK_EQ(iio_block_enqueue(b[0], 0, false), 0, "re-enqueue block 0");
	CHECK_EQ(iio_block_enqueue(b[1], 0, false), 0, "re-enqueue block 1");
	CHECK_EQ(iio_block_enqueue(b[2], 0, false), 0, "re-enqueue block 2");
	CHECK_EQ(send_seq(fd, 2006), 0, "send 2006: 2005 lost");
	CHECK_EQ(send_seq(fd, 2007), 0, "send 2007");
	CHECK_EQ(iio_block_dequeue(b[0], false), 0, "block 0 done");
	TEST_ASSERT(block_is_zero(b[0]), "block 0 is the zero-filled datagram 2005");
	CHECK_EQ(iio_block_dequeue(b[1], false), 0, "block 1 done");
	TEST_ASSERT(block_is(b[1], 2006), "block 1 is datagram 2006");
	CHECK_EQ(iio_block_dequeue(b[2], false), 0, "block 2 done");
	TEST_ASSERT(block_is(b[2], 2007), "block 2 is datagram 2007");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_foreign_packets"), 1, "1 foreign");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_malformed"), 1, "1 malformed");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_duplicate_packets"), 1, "1 duplicate");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_late_packets"), 1, "1 late");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_lost_packets"), 1999 + 1, "late one taken back");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_zero_filled_bytes"), PAYLOAD, "one payload of zeros");

	/* A new enable has a new setup: the sequence starts over at 0 */
	CHECK_EQ(iio_buffer_stream_stop(rx.stream), 0, "disable");
	CHECK_EQ(iio_block_enqueue(b[3], 0, false), 0, "enqueue while disabled");
	CHECK_EQ(iio_buffer_stream_start(rx.stream), 0, "enable again");
	CHECK_EQ(send_seq(fd, 0), 0, "send 0");
	CHECK_EQ(iio_block_dequeue(b[3], false), 0, "block 3 done after re-enable");
	TEST_ASSERT(block_is(b[3], 0), "block 3 is datagram 0 of the new stream");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_duplicate_packets"), 1, "not a duplicate");
	CHECK_EQ(attr_ll(rx.buf, "daqiri_rx_late_packets"), 1, "not late");
out:
	rx_destroy(&rx);
	close(fd);
}

/* --mixed, against float_scan_elements.xml: float-dev voltage1 is le:F16/16,
 * voltage2 le:f32/32 */
TEST_FUNCTION(mixed_lengths)
{
	static const char *const mixed[] = { "voltage1", "voltage2" };
	struct rx rx;
	int ret;

	ret = rx_init_chans(&rx, ctx_for(yaml), "float-dev", mixed, false);
	CHECK_EQ(ret, 0, "16 + 32-bit mask");
	if (!ret)
		CHECK_EQ(rx_open(&rx, 0), 0, "the device's own layout opens");
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
	RUN_TEST(payload_not_whole_samples);
	RUN_TEST(data_smoke);
out:
	printf("test_daqiri_lifecycle: %d/%d passed, %d failed\n", test_passed, test_count,
			test_failed);

	return test_failed ? 1 : 0;
}
