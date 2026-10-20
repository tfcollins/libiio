// SPDX-License-Identifier: MIT
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Unit test for dq_vrt_rx_header(), the sequence tracker entry point for a
 * packet that header-data split (HDS) cut in two: the VRT prologue must be
 * in the header part, the size word must match the total length. Uses the
 * if_data_sid_full.bin golden vector (CI16, SID, Class ID, TSI, TSF: a
 * 28-byte prologue and 1440 payload bytes).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daqiri-vrt-stream.h"
#include "test_framework.h"

#define PROLOGUE 28u
#define PAYLOAD 1440u
#define SID 0x01000000u

/* Returns a malloc'd buffer of exactly the file size (never zero-sized). */
static uint8_t *load(const char *name, size_t *len)
{
	char path[1024];
	uint8_t *buf;
	long sz;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", TESTS_VRT_DIR, name);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(sz ? sz : 1);
	if (buf && fread(buf, 1, sz, f) != (size_t)sz) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = sz;
	return buf;
}

/* An exactly sized heap copy of the first n bytes: ASan catches over-reads */
static uint8_t *head(const uint8_t *p, size_t n)
{
	uint8_t *h = malloc(n ? n : 1);

	if (h)
		memcpy(h, p, n);
	return h;
}

static void init_rx(struct dq_vrt_rx *rx)
{
	struct dq_vrt_cfg cfg = { 0 };

	cfg.gap_ts = true;
	cfg.sample_size = 4;
	dq_vrt_rx_init(rx, &cfg);
}

static bool counters_zero(const struct dq_vrt_counters *c)
{
	static const struct dq_vrt_counters zero;

	return !memcmp(c, &zero, sizeof(zero));
}

TEST_FUNCTION(split_boundaries)
{
	static const size_t bounds[] = { 0, 10, 27, 28, 29, 70, 0 /* len */ };
	struct dq_vrt_rx ref_rx, rx;
	struct dq_vrt_data ref, out, sentinel;
	size_t len = 0, i, b;
	uint8_t *pkt = load("if_data_sid_full.bin", &len), *hdr;
	char msg[128];
	int ret;

	TEST_ASSERT_PTR_NOT_NULL(pkt, "load if_data_sid_full.bin");
	if (!pkt)
		return;
	TEST_ASSERT_EQ(len, PROLOGUE + PAYLOAD, "vector length");

	/* The single-buffer parse every split is compared with */
	init_rx(&ref_rx);
	ret = dq_vrt_rx_packet(&ref_rx, pkt, len, &ref);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "reference: DATA");
	TEST_ASSERT_EQ(ref.len, PAYLOAD, "reference: payload length");
	TEST_ASSERT(ref.payload == pkt + PROLOGUE, "reference: payload after the prologue");
	TEST_ASSERT_EQ(ref_rx.sid, SID, "reference: stream id");

	memset(&sentinel, 0x5A, sizeof(sentinel));

	for (i = 0; i < sizeof(bounds) / sizeof(bounds[0]); i++) {
		b = bounds[i] ? bounds[i] : len;
		hdr = head(pkt, b);
		TEST_ASSERT_PTR_NOT_NULL(hdr, "alloc header part");
		if (!hdr)
			break;

		init_rx(&rx);
		out = sentinel;
		ret = dq_vrt_rx_header(&rx, hdr, b, len, &out);

		if (b < PROLOGUE) {
			snprintf(msg, sizeof(msg), "split at %zu: -EPROTO", b);
			TEST_ASSERT_EQ(ret, -EPROTO, msg);
			snprintf(msg, sizeof(msg), "split at %zu: out untouched", b);
			TEST_ASSERT(!memcmp(&out, &sentinel, sizeof(out)), msg);
			snprintf(msg, sizeof(msg), "split at %zu: nothing counted", b);
			TEST_ASSERT(counters_zero(&rx.c), msg);
			snprintf(msg, sizeof(msg), "split at %zu: no stream id lock", b);
			TEST_ASSERT(!rx.sid_locked, msg);
			snprintf(msg, sizeof(msg), "split at %zu: no sequence state", b);
			TEST_ASSERT_EQ(rx.expected_count, -1, msg);
		} else {
			snprintf(msg, sizeof(msg), "split at %zu: DATA", b);
			TEST_ASSERT_EQ(ret, DQ_VRT_DATA, msg);
			snprintf(msg, sizeof(msg), "split at %zu: payload length", b);
			TEST_ASSERT_EQ(out.len, ref.len, msg);
			snprintf(msg, sizeof(msg), "split at %zu: payload offset", b);
			TEST_ASSERT(out.payload == hdr + (ref.payload - pkt), msg);
			snprintf(msg, sizeof(msg), "split at %zu: gap", b);
			TEST_ASSERT_EQ(out.gap, ref.gap, msg);
			snprintf(msg, sizeof(msg), "split at %zu: gap_unknown", b);
			TEST_ASSERT_EQ(out.gap_unknown, ref.gap_unknown, msg);
			snprintf(msg, sizeof(msg), "split at %zu: stream id", b);
			TEST_ASSERT(rx.sid_locked && rx.sid == ref_rx.sid, msg);
			snprintf(msg, sizeof(msg), "split at %zu: packet count", b);
			TEST_ASSERT_EQ(rx.expected_count, ref_rx.expected_count, msg);
			snprintf(msg, sizeof(msg), "split at %zu: counters", b);
			TEST_ASSERT(!memcmp(&rx.c, &ref_rx.c, sizeof(rx.c)), msg);
		}
		free(hdr);
	}

	free(pkt);
}

/* The VRT size word must account for exactly the total length */
TEST_FUNCTION(total_length)
{
	struct dq_vrt_rx rx;
	struct dq_vrt_data out;
	size_t len = 0;
	uint8_t *pkt = load("if_data_sid_full.bin", &len), *hdr;
	int ret;

	TEST_ASSERT_PTR_NOT_NULL(pkt, "load if_data_sid_full.bin");
	if (!pkt)
		return;
	hdr = head(pkt, 70);
	TEST_ASSERT_PTR_NOT_NULL(hdr, "alloc header part");
	if (!hdr) {
		free(pkt);
		return;
	}

	init_rx(&rx);
	ret = dq_vrt_rx_header(&rx, hdr, 70, len + 4, &out);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "split, 4 bytes more than the size word: SKIP");
	ret = dq_vrt_rx_header(&rx, hdr, 70, len - 4, &out);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "split, 4 bytes less than the size word: SKIP");
	TEST_ASSERT_EQ(rx.c.malformed, 2, "size mismatch: malformed");
	TEST_ASSERT_EQ(rx.c.packets, 0, "size mismatch: packets");
	TEST_ASSERT(!rx.sid_locked, "size mismatch: no lock");

	/* Unsplit, the same rule: dq_vrt_rx_packet() is the wrapper */
	init_rx(&rx);
	ret = dq_vrt_rx_header(&rx, pkt, len - 4, len - 4, &out);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "unsplit, truncated: SKIP");
	TEST_ASSERT_EQ(rx.c.malformed, 1, "unsplit, truncated: malformed");
	ret = dq_vrt_rx_header(&rx, pkt, len, len, &out);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "unsplit, exact: DATA");

	free(hdr);
	free(pkt);
}

int main(void)
{
	RUN_TEST(split_boundaries);
	RUN_TEST(total_length);
	TEST_SUMMARY();
}
