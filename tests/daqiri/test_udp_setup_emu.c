/* SPDX-License-Identifier: MIT */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * IIOD_OP_UDP_STREAM_SETUP against iiod-emu, which registers no UDP stream
 * provider: the server must answer -ENOTSUP and keep the connection usable,
 * where a server without the command drops it. Talks to the server through
 * the iiod-client API over a plain TCP socket, as the daqiri: backend does.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <iio/iio.h>
#include <iio/iiod-client.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../iiod-udp-stream.h"
#include "test_framework.h"

#define EMU_PORT 30431 /* with_emu.sh */
#define EMU_DEVICE "cf-ad9361-lpc"
#define BLOCK_SIZE 1024

/* A plain TCP transport for iiod-client, after daqiri-mgmt.c */
struct conn {
	int fd;
};

static ssize_t conn_io(struct conn *c, void *data, size_t len, int timeout_ms, bool wr)
{
	struct pollfd pfd = { .fd = c->fd, .events = wr ? POLLOUT : POLLIN };
	ssize_t ret;

	for (;;) {
		ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			return -errno;
		if (!ret)
			return -ETIMEDOUT;

		if (wr)
			ret = send(c->fd, data, len, 0);
		else
			ret = recv(c->fd, data, len, 0);
		if (ret > 0)
			return ret;
		if (!ret)
			return -EPIPE;
		if (errno != EINTR && errno != EAGAIN)
			return -errno;
	}
}

static ssize_t conn_read(struct iiod_client_pdata *desc, char *dst, size_t len, int timeout_ms)
{
	return conn_io((struct conn *)desc, dst, len, timeout_ms, false);
}

static ssize_t conn_write(
		struct iiod_client_pdata *desc, const char *src, size_t len, int timeout_ms)
{
	return conn_io((struct conn *)desc, (void *)src, len, timeout_ms, true);
}

/* Wakes the client's reader thread so that iiod_client_destroy() can join it */
static void conn_cancel(struct iiod_client_pdata *desc)
{
	shutdown(((struct conn *)desc)->fd, SHUT_RDWR);
}

static const struct iiod_client_ops conn_ops = {
	.write = conn_write,
	.read = conn_read,
	.cancel = conn_cancel,
};

static int conn_open(struct conn *c)
{
	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_port = htons(EMU_PORT),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	int err, yes = 1;

	c->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (c->fd < 0)
		return -errno;

	if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
			setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0) {
		err = -errno;
		close(c->fd);
		return err;
	}

	return 0;
}

/* iiod_client_create_context() needs a backend for the context it builds.
 * Nothing in this test goes through it. */
static const struct iio_backend_ops test_backend_ops = { 0 };

static const struct iio_backend test_backend = {
	.api_version = IIO_BACKEND_API_V1,
	.name = "udp-setup-test",
	.uri_prefix = "udp-setup-test:",
	.ops = &test_backend_ops,
	.default_timeout_ms = 5000,
};

static const struct iio_context_params params = {
	.log_level = LEVEL_WARNING,
	.timeout_ms = 5000,
};

static const struct iiod_udp_stream_req req = {
	.dst_ip = 0x7f000001, /* 127.0.0.1 */
	.dst_port = 25101,
	.max_payload = 8960,
};

/* TEST_ASSERT_EQ() evaluates its first argument twice with TESTS_DEBUG, so
 * every call below goes through <ret>. */
static void udp_setup(struct iiod_client *client, const struct iio_device *dev,
		struct iio_channels_mask *mask)
{
	struct iiod_client_buffer_pdata *buf;
	struct iiod_udp_stream_resp resp;
	struct iio_block_pdata *block;
	void *data;
	int ret;

	buf = iiod_client_open_buffer(client, client, dev, 0, mask);
	ret = iio_err(buf);
	TEST_ASSERT_EQ(ret, 0, "buffer opened");
	if (ret)
		return;

	ret = iiod_client_udp_stream_setup(buf, &req, &resp);
	TEST_ASSERT_EQ(ret, -ENOTSUP, "setup refused: no provider");
	ret = iiod_client_nop(client);
	TEST_ASSERT_EQ(ret, 0, "connection usable after the refusal");

	/* The server cannot enable a buffer that has no block */
	block = iiod_client_create_block(buf, BLOCK_SIZE, &data);
	ret = iio_err(block);
	TEST_ASSERT_EQ(ret, 0, "block created");
	if (ret) {
		iiod_client_close_buffer(buf);
		return;
	}

	ret = iiod_client_enable_buffer(buf, 0, true, false);
	TEST_ASSERT_EQ(ret, 0, "buffer enabled");

	/* With a provider, a second enable starts nothing; without one, it is a
	 * plain enable of an enabled buffer */
	ret = iiod_client_enable_buffer(buf, 0, true, false);
	TEST_ASSERT_EQ(ret, 0, "buffer enabled again");

	/* With a provider and an active stream this would be -EBUSY; without
	 * one, -ENOTSUP comes first */
	ret = iiod_client_udp_stream_setup(buf, &req, &resp);
	TEST_ASSERT_EQ(ret, -ENOTSUP, "setup refused while enabled");

	/* Samples still flow over TCP, as after a data=auto fallback */
	ret = iiod_client_enqueue_block(block, BLOCK_SIZE, false);
	TEST_ASSERT_EQ(ret, 0, "block enqueued");
	ret = iiod_client_dequeue_block(block, false);
	TEST_ASSERT_EQ(ret, 0, "block dequeued");

	ret = iiod_client_enable_buffer(buf, 0, false, false);
	TEST_ASSERT_EQ(ret, 0, "buffer disabled");

	iiod_client_free_block(block);
	iiod_client_close_buffer(buf);

	/* Closing reports nothing; re-opening the same buffer, which the server
	 * refuses with -EEXIST while it is still open, shows that it is closed */
	buf = iiod_client_open_buffer(client, client, dev, 0, mask);
	ret = iio_err(buf);
	TEST_ASSERT_EQ(ret, 0, "buffer closed, then re-opened");
	if (!ret)
		iiod_client_close_buffer(buf);

	ret = iiod_client_nop(client);
	TEST_ASSERT_EQ(ret, 0, "connection usable after close");
}

int main(void)
{
	const struct iio_device *dev;
	struct iio_channels_mask *mask;
	struct iiod_client *client;
	struct iio_context *ctx;
	struct conn conn;
	int ret;

	/* A server that drops the connection must fail the test, not kill it */
	signal(SIGPIPE, SIG_IGN);

	ret = conn_open(&conn);
	if (ret) {
		printf("Unable to connect to 127.0.0.1:%d: %d\n", EMU_PORT, ret);
		return 1;
	}

	client = iiod_client_new(&params, (struct iiod_client_pdata *)&conn, &conn_ops);
	if (iio_err(client)) {
		printf("Unable to create the iiod client: %d\n", iio_err(client));
		close(conn.fd);
		return 1;
	}

	TEST_ASSERT(iiod_client_uses_binary_interface(client), "binary protocol");

	ctx = iiod_client_create_context(client, &test_backend, NULL, NULL, NULL, 0);
	TEST_ASSERT_EQ(iio_err(ctx), 0, "context created");
	if (iio_err(ctx)) {
		ctx = NULL;
		goto out_destroy_client;
	}

	dev = iio_context_find_device(ctx, EMU_DEVICE);
	TEST_ASSERT_PTR_NOT_NULL(dev, EMU_DEVICE " found");
	if (!dev)
		goto out_destroy_client;

	mask = iio_create_channels_mask(iio_device_get_channels_count(dev));
	TEST_ASSERT_PTR_NOT_NULL(mask, "mask created");
	if (!mask)
		goto out_destroy_client;

	iio_channel_enable(iio_device_find_channel(dev, "voltage0", false), mask);
	iio_channel_enable(iio_device_find_channel(dev, "voltage1", false), mask);

	udp_setup(client, dev, mask);

	iio_channels_mask_destroy(mask);

out_destroy_client:
	/* The client logs through the context's params: destroy it first */
	iiod_client_destroy(client);
	close(conn.fd);
	if (ctx)
		iio_context_destroy(ctx);

	printf("test_udp_setup_emu: %d/%d passed, %d failed\n", test_passed, test_count,
			test_failed);

	return test_failed ? 1 : 0;
}
