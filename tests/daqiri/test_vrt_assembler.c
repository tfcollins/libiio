// SPDX-License-Identifier: MIT
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Unit test for the DAQIRI block assembler: copy with byte swap, carry of
 * payloads straddling blocks, and gap handling (fail or zero-fill).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daqiri-asm.h"
#include "test_framework.h"

#define SENTINEL 0xA5

static void fill(uint8_t *p, size_t len, unsigned seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (uint8_t)(seed + i * 7);
}

static bool all(const uint8_t *p, size_t len, uint8_t v)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i] != v)
			return false;
	return true;
}

TEST_FUNCTION(three_pushes)
{
	uint8_t src[1440], dst[1440];
	struct dq_asm a;
	bool done = true;
	int ret;

	fill(src, sizeof(src), 1);
	ret = dq_asm_init(&a, DQ_SWAP_NONE, false, 1440);
	TEST_ASSERT_EQ(ret, 0, "init");
	dq_asm_attach(&a, dst, sizeof(dst));
	ret = dq_asm_push(&a, src, 480, 0, &done);
	TEST_ASSERT(ret == 480 && !done, "push 1");
	ret = dq_asm_push(&a, src + 480, 480, 0, &done);
	TEST_ASSERT(ret == 480 && !done, "push 2");
	ret = dq_asm_push(&a, src + 960, 480, 0, &done);
	TEST_ASSERT(ret == 480 && done, "push 3 completes");
	TEST_ASSERT_EQ(a.off, 1440, "off");
	TEST_ASSERT_EQ(a.carry_len, 0, "no carry");
	TEST_ASSERT(!memcmp(dst, src, sizeof(dst)), "dst equals input");
	TEST_ASSERT(!a.had_gap, "no gap");
	dq_asm_fini(&a);
	TEST_ASSERT_PTR_NULL(a.carry, "fini frees carry");
}

TEST_FUNCTION(swap)
{
	static const uint8_t be[] = { 0x00, 0x01, 0x00, 0x02 };
	static const uint8_t s16[] = { 0x01, 0x00, 0x02, 0x00 };
	static const uint8_t s32[] = { 0x02, 0x00, 0x01, 0x00 };
	uint8_t dst[4];
	struct dq_asm a;
	bool done;
	int ret;

	dq_asm_init(&a, DQ_SWAP_16, false, 0);
	dq_asm_attach(&a, dst, sizeof(dst));
	ret = dq_asm_push(&a, be, sizeof(be), 0, &done);
	TEST_ASSERT(ret == 4 && done, "swap16: push");
	TEST_ASSERT(!memcmp(dst, s16, sizeof(dst)), "swap16: 01 00 02 00");
	dq_asm_fini(&a);

	dq_asm_init(&a, DQ_SWAP_32, false, 0);
	dq_asm_attach(&a, dst, sizeof(dst));
	ret = dq_asm_push(&a, be, sizeof(be), 0, &done);
	TEST_ASSERT(ret == 4 && done, "swap32: push");
	TEST_ASSERT(!memcmp(dst, s32, sizeof(dst)), "swap32: 02 00 01 00");

	/* Odd length for the swap unit is rejected, nothing copied. */
	dq_asm_attach(&a, dst, sizeof(dst));
	ret = dq_asm_push(&a, be, 2, 0, &done);
	TEST_ASSERT_EQ(ret, -EINVAL, "swap32: 2 bytes -EINVAL");
	TEST_ASSERT_EQ(a.off, 0, "swap32: -EINVAL copies nothing");
	dq_asm_fini(&a);
}

TEST_FUNCTION(straddle)
{
	uint8_t src[1200], dst[1000], dst2[1000];
	struct dq_asm a;
	bool done = true;
	int ret;

	fill(src, sizeof(src), 3);
	dq_asm_init(&a, DQ_SWAP_NONE, false, 1024);
	dq_asm_attach(&a, dst, sizeof(dst));
	ret = dq_asm_push(&a, src, 600, 0, &done);
	TEST_ASSERT(ret == 600 && !done, "straddle: first 600");
	ret = dq_asm_push(&a, src + 600, 600, 0, &done);
	TEST_ASSERT_EQ(ret, 400, "straddle: second writes 400");
	TEST_ASSERT(done, "straddle: block done");
	TEST_ASSERT_EQ(a.carry_len, 200, "straddle: carry 200");
	TEST_ASSERT(!memcmp(dst, src, 1000), "straddle: block content");

	dq_asm_attach(&a, dst2, sizeof(dst2));
	TEST_ASSERT_EQ(a.off, 200, "attach: carry flushed");
	TEST_ASSERT_EQ(a.carry_len, 0, "attach: carry empty");
	TEST_ASSERT(!memcmp(dst2, src + 1000, 200), "attach: carry content");
	dq_asm_fini(&a);
}

TEST_FUNCTION(straddle_swap)
{
	uint8_t src[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	static const uint8_t exp[8] = { 3, 2, 1, 0, 7, 6, 5, 4 };
	uint8_t dst[4], dst2[4];
	struct dq_asm a;
	bool done;
	int ret;

	/* The carried half is swapped exactly once. */
	dq_asm_init(&a, DQ_SWAP_32, false, 4);
	dq_asm_attach(&a, dst, sizeof(dst));
	ret = dq_asm_push(&a, src, 8, 0, &done);
	TEST_ASSERT(ret == 4 && done, "straddle swap: push");
	dq_asm_attach(&a, dst2, sizeof(dst2));
	TEST_ASSERT(!memcmp(dst, exp, 4) && !memcmp(dst2, exp + 4, 4), "straddle swap: content");
	dq_asm_fini(&a);
}

TEST_FUNCTION(attach_fills_block)
{
	uint8_t src[1250], dst[1000], small[100], dst3[1000];
	struct dq_asm a;
	bool done = false;
	int ret;

	/* Leave 200 bytes of carry, then attach a block smaller than it. */
	fill(src, sizeof(src), 5);
	dq_asm_init(&a, DQ_SWAP_NONE, false, 1024);
	dq_asm_attach(&a, dst, sizeof(dst));
	dq_asm_push(&a, src, 600, 0, &done);
	dq_asm_push(&a, src + 600, 600, 0, &done);
	dq_asm_attach(&a, small, sizeof(small));
	TEST_ASSERT_EQ(a.off, 100, "small: filled by carry");
	TEST_ASSERT_EQ(a.carry_len, 100, "small: carry left");
	TEST_ASSERT(!memcmp(small, src + 1000, 100), "small: content");

	/* The next push reports the block done and carries its payload. */
	done = false;
	ret = dq_asm_push(&a, src + 1200, 50, 0, &done);
	TEST_ASSERT(ret == 0 && done, "full block: push returns 0, done");
	TEST_ASSERT_EQ(a.carry_len, 150, "full block: payload carried");
	dq_asm_attach(&a, dst3, sizeof(dst3));
	TEST_ASSERT_EQ(a.off, 150, "next block: carry flushed");
	TEST_ASSERT(!memcmp(dst3, src + 1100, 150), "next block: order kept");
	dq_asm_fini(&a);
}

TEST_FUNCTION(gap_strict)
{
	uint8_t src[480], dst[1440];
	struct dq_asm a;
	bool done = true;
	int ret;

	fill(src, sizeof(src), 7);
	memset(dst, SENTINEL, sizeof(dst));
	dq_asm_init(&a, DQ_SWAP_NONE, false, 1440);
	dq_asm_attach(&a, dst, sizeof(dst));
	dq_asm_push(&a, src, 480, 0, &done);
	ret = dq_asm_push(&a, src, 480, 1, &done);
	TEST_ASSERT_EQ(ret, -EIO, "strict gap: -EIO");
	TEST_ASSERT(a.had_gap, "strict gap: had_gap");
	TEST_ASSERT_EQ(a.off, 480, "strict gap: off unchanged");
	TEST_ASSERT(all(dst + 480, 960, SENTINEL), "strict gap: nothing written");
	TEST_ASSERT_EQ(a.carry_len, 0, "strict gap: nothing carried");
	TEST_ASSERT_EQ(a.zero_filled, 0, "strict gap: no zero fill");

	dq_asm_attach(&a, dst, sizeof(dst));
	TEST_ASSERT(!a.had_gap, "attach clears had_gap");
	dq_asm_fini(&a);
}

TEST_FUNCTION(gap_lossy)
{
	uint8_t src[1200], dst[1440], dst2[1440];
	struct dq_asm a;
	bool done = false;
	int ret;

	fill(src, sizeof(src), 9);
	memset(dst, SENTINEL, sizeof(dst));
	dq_asm_init(&a, DQ_SWAP_NONE, true, 1440);
	dq_asm_attach(&a, dst, sizeof(dst));
	ret = dq_asm_push(&a, src, 480, 2, &done);
	TEST_ASSERT_EQ(ret, 480, "lossy: payload written");
	TEST_ASSERT(done, "lossy: done");
	TEST_ASSERT_EQ(a.zero_filled, 960, "lossy: zero_filled 960");
	TEST_ASSERT(a.had_gap, "lossy: had_gap");
	TEST_ASSERT(all(dst, 960, 0), "lossy: zeros");
	TEST_ASSERT(!memcmp(dst + 960, src, 480), "lossy: payload after");

	/* At off 1200 only 240 zero bytes fit; the payload is carried. */
	memset(dst2, SENTINEL, sizeof(dst2));
	dq_asm_attach(&a, dst2, sizeof(dst2));
	TEST_ASSERT(!a.had_gap, "lossy: attach clears had_gap");
	dq_asm_push(&a, src, 1200, 0, &done);
	ret = dq_asm_push(&a, src, 480, 1, &done);
	TEST_ASSERT(ret == 0 && done, "clamped: 0 written, done");
	TEST_ASSERT_EQ(a.zero_filled, 960 + 240, "clamped: zero_filled +240");
	TEST_ASSERT(all(dst2 + 1200, 240, 0), "clamped: zeros");
	TEST_ASSERT_EQ(a.carry_len, 480, "clamped: payload to carry");

	dq_asm_attach(&a, dst, sizeof(dst));
	TEST_ASSERT_EQ(a.off, 480, "clamped: carry flushed");
	TEST_ASSERT(!memcmp(dst, src, 480), "clamped: carry content");
	TEST_ASSERT_EQ(a.zero_filled, 1200, "attach keeps zero_filled");
	dq_asm_fini(&a);
}

TEST_FUNCTION(carry_overflow)
{
	uint8_t src[1440], dst[1440];
	struct dq_asm a;
	bool done = false;
	int ret;

	fill(src, sizeof(src), 11);
	memset(dst, SENTINEL, sizeof(dst));
	dq_asm_init(&a, DQ_SWAP_NONE, false, 100);
	dq_asm_attach(&a, dst, 1000);
	dq_asm_push(&a, src, 600, 0, &done);
	ret = dq_asm_push(&a, src, 600, 0, &done);
	TEST_ASSERT_EQ(ret, -ENOBUFS, "overflow: -ENOBUFS");
	TEST_ASSERT_EQ(a.off, 600, "overflow: off unchanged");
	TEST_ASSERT_EQ(a.carry_len, 0, "overflow: carry unchanged");
	TEST_ASSERT(all(dst + 600, 840, SENTINEL), "overflow: nothing written");
	dq_asm_fini(&a);

	/* Lossy: checked before the zero fill too. */
	memset(dst, SENTINEL, sizeof(dst));
	dq_asm_init(&a, DQ_SWAP_NONE, true, 100);
	dq_asm_attach(&a, dst, sizeof(dst));
	dq_asm_push(&a, src, 1200, 0, &done);
	ret = dq_asm_push(&a, src, 480, 1, &done);
	TEST_ASSERT_EQ(ret, -ENOBUFS, "lossy overflow: -ENOBUFS");
	TEST_ASSERT(all(dst + 1200, 240, SENTINEL), "lossy overflow: no zeros");
	TEST_ASSERT_EQ(a.zero_filled, 0, "lossy overflow: zero_filled");
	TEST_ASSERT(!a.had_gap, "lossy overflow: had_gap");
	TEST_ASSERT_EQ(a.off, 1200, "lossy overflow: off");
	dq_asm_fini(&a);
}

TEST_FUNCTION(not_attached)
{
	uint8_t src[4] = { 0 };
	struct dq_asm a;
	bool done;
	int ret;

	dq_asm_init(&a, DQ_SWAP_NONE, false, 16);
	ret = dq_asm_push(&a, src, 4, 0, &done);
	TEST_ASSERT_EQ(ret, -EINVAL, "push before attach: -EINVAL");
	dq_asm_fini(&a);
}

int main(void)
{
	RUN_TEST(three_pushes);
	RUN_TEST(swap);
	RUN_TEST(straddle);
	RUN_TEST(straddle_swap);
	RUN_TEST(attach_fills_block);
	RUN_TEST(gap_strict);
	RUN_TEST(gap_lossy);
	RUN_TEST(carry_overflow);
	RUN_TEST(not_attached);
	TEST_SUMMARY();
}
