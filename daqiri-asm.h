/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Block assembler for the DAQIRI backend: copies packet payloads into the
 * current block with an optional byte swap, carries the bytes that do not
 * fit over to the next block, and fails or zero-fills on packet loss. Free
 * of libiio, DAQIRI and CUDA dependencies so it can be unit tested alone.
 *
 * Payload lengths and block sizes must be multiples of the swap unit (2 for
 * DQ_SWAP_16, 4 for DQ_SWAP_32) so that no swapped word straddles a block;
 * dq_asm_push() enforces both.
 * Blocks should be at least as large as the largest payload: the carry then
 * never holds more than one payload. A carry larger than the block fills
 * the block in dq_asm_attach(); the next dq_asm_push() then returns 0 with
 * *done = true and appends its payload to the carry.
 */

#ifndef DAQIRI_ASM_H
#define DAQIRI_ASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dq_swap { DQ_SWAP_NONE = 0, DQ_SWAP_16 = 2, DQ_SWAP_32 = 4 };

struct dq_asm {
	uint8_t *dst; /* current block, NULL until attached */
	size_t size, off;
	enum dq_swap swap;
	bool lossy;
	uint8_t *carry; /* swapped bytes waiting for the next block */
	size_t carry_cap, carry_len;
	uint64_t zero_filled; /* cumulative, never reset */
	bool had_gap;	      /* current block; reset by dq_asm_attach() */
};

/* Returns 0, -EINVAL for an unknown swap, -ENOMEM if the carry malloc fails. */
int dq_asm_init(struct dq_asm *a, enum dq_swap swap, bool lossy, size_t carry_cap);
void dq_asm_fini(struct dq_asm *a);

/*
 * Make dst (size bytes) the current block: off = 0, had_gap = false, then
 * the carry is flushed into it (up to size bytes; any rest stays carried).
 * A size that is not a multiple of the swap unit gets nothing flushed and
 * every dq_asm_push() into it fails with -EINVAL.
 */
void dq_asm_attach(struct dq_asm *a, void *dst, size_t size);

/*
 * Append one payload to the current block; gap_packets packets were lost
 * right before it.
 *  - gap_packets > 0 && !lossy: returns -EIO without copying and sets
 *    had_gap; the block is failed. The caller attaches the next block and
 *    pushes the payload again with gap_packets = 0.
 *  - gap_packets > 0 && lossy: zero-fills min(gap_packets * len, size - off)
 *    bytes (zeros that do not fit are dropped), zero_filled += that,
 *    had_gap = true, then copies the payload.
 * The copy applies the swap. Bytes that do not fit go to the carry, so on
 * success the whole payload is consumed: the return value is the number of
 * bytes written to the block (0..len), the rest was carried.
 * *done = (off == size), set on success only.
 * Errors, all with nothing written or counted:
 *  -EIO     gap with lossy off (had_gap is set);
 *  -ENOBUFS the carry cannot take the bytes that do not fit;
 *  -EINVAL  no block attached, or len or the block size not a multiple of
 *           the swap unit.
 */
int dq_asm_push(struct dq_asm *a, const uint8_t *payload, size_t len, uint32_t gap_packets,
		bool *done);

#ifdef __cplusplus
}
#endif

#endif /* DAQIRI_ASM_H */
