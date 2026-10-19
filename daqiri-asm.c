// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Block assembler for the DAQIRI backend; see daqiri-asm.h.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "daqiri-asm.h"

int dq_asm_init(struct dq_asm *a, enum dq_swap swap, bool lossy, size_t carry_cap)
{
	memset(a, 0, sizeof(*a));
	if (swap != DQ_SWAP_NONE && swap != DQ_SWAP_16 && swap != DQ_SWAP_32)
		return -EINVAL;

	if (carry_cap) {
		a->carry = malloc(carry_cap);
		if (!a->carry)
			return -ENOMEM;
	}
	a->carry_cap = carry_cap;
	a->swap = swap;
	a->lossy = lossy;
	return 0;
}

void dq_asm_fini(struct dq_asm *a)
{
	free(a->carry);
	memset(a, 0, sizeof(*a));
}

/* Byte-wise so that unaligned packet payloads are fine. A trailing partial
 * word (only if the caller broke the size rules) is copied unswapped. */
static void dq_asm_copy(uint8_t *d, const uint8_t *s, size_t n, enum dq_swap swap)
{
	size_t i = 0;

	if (swap == DQ_SWAP_16) {
		for (; i + 2 <= n; i += 2) {
			d[i] = s[i + 1];
			d[i + 1] = s[i];
		}
	} else if (swap == DQ_SWAP_32) {
		for (; i + 4 <= n; i += 4) {
			d[i] = s[i + 3];
			d[i + 1] = s[i + 2];
			d[i + 2] = s[i + 1];
			d[i + 3] = s[i];
		}
	}
	if (i < n)
		memcpy(d + i, s + i, n - i);
}

static size_t dq_asm_unit(const struct dq_asm *a)
{
	return a->swap ? (size_t)a->swap : 1;
}

void dq_asm_attach(struct dq_asm *a, void *dst, size_t size)
{
	size_t n = a->carry_len < size ? a->carry_len : size;

	/* A misaligned block would split a swapped word: leave the carry
	 * alone, dq_asm_push() rejects the block. */
	if (size % dq_asm_unit(a))
		n = 0;

	a->dst = dst;
	a->size = size;
	a->off = n;
	a->had_gap = false;
	if (n) {
		memcpy(a->dst, a->carry, n);
		a->carry_len -= n;
		memmove(a->carry, a->carry + n, a->carry_len);
	}
}

int dq_asm_push(struct dq_asm *a, const uint8_t *payload, size_t len, uint32_t gap_packets,
		bool *done)
{
	size_t unit = dq_asm_unit(a), room, zeros = 0, n, rest;
	uint64_t want;

	if (!a->dst || len % unit || a->size % unit)
		return -EINVAL;
	if (gap_packets && !a->lossy) {
		a->had_gap = true;
		return -EIO;
	}

	/* Size everything first: nothing is written unless it all fits. */
	room = a->size - a->off;
	if (gap_packets) {
		want = (uint64_t)gap_packets * len;
		zeros = want < room ? (size_t)want : room;
	}
	n = len < room - zeros ? len : room - zeros;
	rest = len - n;
	if (rest > a->carry_cap - a->carry_len)
		return -ENOBUFS;

	if (gap_packets) {
		memset(a->dst + a->off, 0, zeros);
		a->off += zeros;
		a->zero_filled += zeros;
		a->had_gap = true;
	}
	if (n) {
		dq_asm_copy(a->dst + a->off, payload, n, a->swap);
		a->off += n;
	}
	if (rest) {
		dq_asm_copy(a->carry + a->carry_len, payload + n, rest, a->swap);
		a->carry_len += rest;
	}

	*done = a->off == a->size;
	return (int)n;
}
