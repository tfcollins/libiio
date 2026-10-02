// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdint>
#include <cstring>

#include "daqiri-private.h"

namespace dq
{
// CONFIG body: big-endian u32 actual RX block extent (zero for TX).
// Exact CONFIG echo must precede START; max_block in HELLO is only a ceiling.
enum Kind {
	HELLO = 1,
	START = 2,
	STOP = 3,
	ACK = 4,
	CREDIT = 5,
	DATA = 6,
	ERROR = 7,
	CONFIG = 8,
};

inline void put(void *p, uint64_t v, unsigned n)
{
	auto b = static_cast<unsigned char *>(p);

	while (n) {
		b[--n] = v & 255;
		v >>= 8;
	}
}

inline uint64_t get(const void *p, unsigned n)
{
	auto b = static_cast<const unsigned char *>(p);
	uint64_t v = 0;

	while (n--)
		v = (v << 8) | *b++;
	return v;
}

struct Packet {
	unsigned kind;
	uint64_t epoch;
	uint64_t seq;
	uint64_t value;
	const unsigned char *payload;
	size_t size;
};

inline size_t encode(void *dst, unsigned kind, uint64_t epoch, uint64_t seq, uint64_t value,
        const void *payload = nullptr, size_t size = 0)
{
	auto b = static_cast<unsigned char *>(dst);

	std::memset(b, 0, DQ_HEADER);
	std::memcpy(b, "DQI1", 4);
	b[4] = 1;
	b[5] = static_cast<unsigned char>(kind);
	put(b + 8, epoch, 8);
	put(b + 16, seq, 8);
	put(b + 24, value, 8);
	put(b + 32, size, 4);
	if (size)
		std::memcpy(b + DQ_HEADER, payload, size);
	return DQ_HEADER + size;
}

inline bool decode(const void *src, size_t len, Packet &p)
{
	auto b = static_cast<const unsigned char *>(src);

	if (len < DQ_HEADER || len > DQ_MAX_PACKET || std::memcmp(b, "DQI1", 4) || b[4] != 1 ||
	        b[5] < HELLO || b[5] > CONFIG || get(b + 6, 2) || get(b + 36, 4) ||
	        get(b + 32, 4) != len - DQ_HEADER)
		return false;
	p = { b[5], get(b + 8, 8), get(b + 16, 8), get(b + 24, 8), b + DQ_HEADER, len - DQ_HEADER };
	return p.epoch != 0;
}

}
