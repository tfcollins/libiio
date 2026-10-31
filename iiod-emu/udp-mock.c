// SPDX-License-Identifier: MIT
/*
 * iiod-emu - IIO hardware emulator
 *
 * A UDP stream provider that sends nothing and writes one line per call
 * instead, for the test of the responder's rules for providers
 * (tests/daqiri/test_udp_provider_contract.c). Hidden option
 * --udp-mock-provider <log>.
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 */

#include <errno.h>
#include <iio/iio-lock.h>
#include <iio/iio.h>
#include <iiod/ops.h>
#include <iiod/udp-stream.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "udp-stream.h"

/* max_payload values that make the mock misbehave on purpose */
#define SETUP_RETURNS_POSITIVE 4321 /* setup() returns 5 */
#define START_FAILS 1234	    /* start() fails with -EADDRINUSE */

struct emu_udp_mock {
	FILE *log;
	struct iio_mutex *lock;
};

/* In entry->udp_priv */
struct emu_udp_mock_buf {
	uint16_t max_payload;
};

static void __attribute__((format(printf, 2, 3))) mock_log(
		struct emu_udp_mock *mock, const char *fmt, ...)
{
	va_list ap;

	iio_mutex_lock(mock->lock);

	va_start(ap, fmt);
	vfprintf(mock->log, fmt, ap);
	va_end(ap);

	fputc('\n', mock->log);
	fflush(mock->log);

	iio_mutex_unlock(mock->lock);
}

static int mock_setup(void *priv, struct buffer_entry *entry, const struct iiod_udp_stream_req *req,
		struct iiod_udp_stream_resp *resp)
{
	struct emu_udp_mock_buf *mb = entry->udp_priv;

	mock_log(priv, "setup %u", req->max_payload);

	if (!mb) {
		mb = calloc(1, sizeof(*mb));
		if (!mb)
			return -ENOMEM;
		entry->udp_priv = mb;
	}

	mb->max_payload = req->max_payload;

	if (req->max_payload == SETUP_RETURNS_POSITIVE)
		return 5;

	resp->stream_id = 0x12345678;
	resp->payload_bytes = req->max_payload & ~15u;
	resp->src_ip = 0x7f000001; /* 127.0.0.1 */
	resp->src_port = 25102;

	return 0;
}

static int mock_start(void *priv, struct buffer_entry *entry)
{
	struct emu_udp_mock_buf *mb = entry->udp_priv;

	/* The responder broke its rules: say so rather than crash */
	if (!mb) {
		mock_log(priv, "start without a setup");
		return -EINVAL;
	}

	if (mb->max_payload == START_FAILS) {
		mock_log(priv, "start failed");
		return -EADDRINUSE;
	}

	mock_log(priv, "start");

	return 0;
}

static void mock_stop(void *priv, struct buffer_entry *entry)
{
	mock_log(priv, "stop");
}

static void mock_release(void *priv, struct buffer_entry *entry)
{
	mock_log(priv, "release");

	free(entry->udp_priv);
	entry->udp_priv = NULL;
}

const struct iiod_udp_stream_ops emu_udp_mock_ops = {
	.setup = mock_setup,
	.start = mock_start,
	.stop = mock_stop,
	.release = mock_release,
};

struct emu_udp_mock *emu_udp_mock_create(const char *path)
{
	struct emu_udp_mock *mock = calloc(1, sizeof(*mock));

	if (!mock)
		return NULL;

	mock->lock = iio_mutex_create();
	if (iio_err(mock->lock))
		goto err_free_mock;

	/* Empty from the start: the test compares the whole file */
	mock->log = fopen(path, "w");
	if (!mock->log) {
		perror(path);
		goto err_destroy_lock;
	}

	return mock;

err_destroy_lock:
	iio_mutex_destroy(mock->lock);
err_free_mock:
	free(mock);
	return NULL;
}

void emu_udp_mock_destroy(struct emu_udp_mock *mock)
{
	if (!mock)
		return;

	fclose(mock->log);
	iio_mutex_destroy(mock->lock);
	free(mock);
}
