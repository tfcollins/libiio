/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * VITA 49.2 prologue parser for the DAQIRI backend. Header-only and free of
 * libiio and DAQIRI dependencies so it can be unit tested on its own.
 *
 * Word 0 (big-endian): type[31:28] C[27] T[26] rsv[25] TSM[24] TSI[23:22]
 * TSF[21:20] pkt_count[19:16] size[15:0]. Optional words follow in this
 * order: Stream ID, Class ID (2 words), integer seconds, fractional
 * timestamp (2 words). A trailer word ends the packet when T=1.
 */

#ifndef DAQIRI_VRT_PROLOGUE_H
#define DAQIRI_VRT_PROLOGUE_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dq_vrt_type {
	DQ_VRT_IF_DATA_NOSID = 0,
	DQ_VRT_IF_DATA_SID = 1,
	DQ_VRT_EXT_DATA_NOSID = 2,
	DQ_VRT_EXT_DATA_SID = 3,
	DQ_VRT_IF_CONTEXT = 4,
	DQ_VRT_EXT_CONTEXT = 5,
	DQ_VRT_COMMAND = 6,
	DQ_VRT_EXT_COMMAND = 7,
};

enum dq_vrt_tsf {
	DQ_VRT_TSF_NONE = 0,
	DQ_VRT_TSF_SAMPLE_COUNT = 1,
	DQ_VRT_TSF_REAL_TIME = 2,
	DQ_VRT_TSF_FREE_RUNNING = 3,
};

struct dq_vrt_prologue {
	uint8_t type;
	bool has_sid, has_class, has_trailer;
	uint8_t tsm, tsi, tsf, pkt_count;
	uint16_t size_words;
	uint32_t stream_id, oui;
	uint16_t info_class, packet_class;
	uint32_t tsi_secs;
	uint64_t tsf_frac;
	unsigned int prologue_bytes;
	unsigned int payload_bytes; /* excludes trailer */
};

static inline uint32_t dq_vrt_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Parse the prologue of the packet at p (len bytes available).
 * Returns 0 on success, -EBADMSG if the packet is truncated, inconsistent, or
 * of a type other than IF data (0/1) or context (4/5).
 */
static inline int dq_vrt_parse(const uint8_t *p, size_t len,
			       struct dq_vrt_prologue *out)
{
	struct dq_vrt_prologue r = { 0 };
	size_t pkt_bytes, off = 4;
	uint32_t w0, w;

	if (len < 4)
		return -EBADMSG;

	w0 = dq_vrt_be32(p);
	r.type = w0 >> 28;
	if (r.type == DQ_VRT_EXT_DATA_NOSID || r.type == DQ_VRT_EXT_DATA_SID ||
	    r.type > DQ_VRT_EXT_CONTEXT)
		return -EBADMSG;

	r.has_class = (w0 >> 27) & 1;
	r.has_trailer = (w0 >> 26) & 1;
	r.tsm = (w0 >> 24) & 1;
	r.tsi = (w0 >> 22) & 3;
	r.tsf = (w0 >> 20) & 3;
	r.pkt_count = (w0 >> 16) & 0xF;
	r.size_words = w0 & 0xFFFF;
	r.has_sid = (r.type & 1) || r.type >= DQ_VRT_IF_CONTEXT;

	pkt_bytes = (size_t)r.size_words * 4;
	if (pkt_bytes > len)
		return -EBADMSG;

	/* Bound every optional word by the packet size, not just by len. */
	if (r.has_sid) {
		if (off + 4 > pkt_bytes)
			return -EBADMSG;
		r.stream_id = dq_vrt_be32(p + off);
		off += 4;
	}
	if (r.has_class) {
		if (off + 8 > pkt_bytes)
			return -EBADMSG;
		w = dq_vrt_be32(p + off);
		r.oui = w & 0xFFFFFF;
		w = dq_vrt_be32(p + off + 4);
		r.info_class = w >> 16;
		r.packet_class = w & 0xFFFF;
		off += 8;
	}
	if (r.tsi) {
		if (off + 4 > pkt_bytes)
			return -EBADMSG;
		r.tsi_secs = dq_vrt_be32(p + off);
		off += 4;
	}
	if (r.tsf) {
		if (off + 8 > pkt_bytes)
			return -EBADMSG;
		r.tsf_frac = ((uint64_t)dq_vrt_be32(p + off) << 32) |
			     dq_vrt_be32(p + off + 4);
		off += 8;
	}

	if (off + (r.has_trailer ? 4 : 0) > pkt_bytes)
		return -EBADMSG;

	r.prologue_bytes = (unsigned int)off;
	r.payload_bytes =
		(unsigned int)(pkt_bytes - off - (r.has_trailer ? 4 : 0));
	*out = r;
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DAQIRI_VRT_PROLOGUE_H */
