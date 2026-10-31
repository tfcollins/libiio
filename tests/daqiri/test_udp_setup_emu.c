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

#include <errno.h>
#include <iio/iio.h>
#include <iio/iiod-client.h>
#include <stdbool.h>
#include <stdio.h>

#include "../../iiod-udp-stream.h"
#include "emu_client.h"
#include "test_framework.h"

#define EMU_DEVICE "cf-ad9361-lpc"
#define BLOCK_SIZE 1024

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
	struct emu_client ec;
	int ret;

	ret = emu_client_open(&ec);
	if (ret)
		return 1;

	TEST_ASSERT(iiod_client_uses_binary_interface(ec.client), "binary protocol");

	/* Without --udp, the emulator does not advertise UDP mode */
	TEST_ASSERT_PTR_NULL(iio_context_find_attr(ec.ctx, "iiod,udp-stream"),
			"no iiod,udp-stream attribute");

	dev = iio_context_find_device(ec.ctx, EMU_DEVICE);
	TEST_ASSERT_PTR_NOT_NULL(dev, EMU_DEVICE " found");
	if (!dev)
		goto out_close;

	mask = iio_create_channels_mask(iio_device_get_channels_count(dev));
	TEST_ASSERT_PTR_NOT_NULL(mask, "mask created");
	if (!mask)
		goto out_close;

	iio_channel_enable(iio_device_find_channel(dev, "voltage0", false), mask);
	iio_channel_enable(iio_device_find_channel(dev, "voltage1", false), mask);

	udp_setup(ec.client, dev, mask);

	iio_channels_mask_destroy(mask);

out_close:
	emu_client_close(&ec);

	printf("test_udp_setup_emu: %d/%d passed, %d failed\n", test_passed, test_count,
			test_failed);

	return test_failed ? 1 : 0;
}
