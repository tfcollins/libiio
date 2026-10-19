/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * VITA 49.2 receive-side sequence tracker for the DAQIRI backend: filters
 * datagrams of one IF data stream and detects packet loss. Free of libiio,
 * DAQIRI and CUDA dependencies so it can be unit tested on its own.
 *
 * Per datagram, in order:
 *  - not parseable (dq_vrt_parse() fails): malformed++, SKIP;
 *  - IF/extension context packet: context++, SKIP;
 *  - Stream ID: with cfg.sid_mask != 0 the packet needs a Stream ID with
 *    (stream_id & sid_mask) == (cfg.sid & sid_mask); without a mask the
 *    first IF data packet locks rx->sid (0 for packets without a Stream ID)
 *    and later packets must carry the same value. Others: foreign++, SKIP;
 *  - format: on the first data packet that gets here, a Class ID with
 *    packet class 0x0010 (vrt49 CI16) needs sample_size 4, packet class
 *    0x0030 (vrt49 REAL_INT16) needs 2; any other class (0 included) or no
 *    Class ID is accepted unchecked.
 *    Every data packet needs payload_bytes % sample_size == 0. A violation
 *    returns -EPROTO and changes no state or counter, so the next accepted
 *    packet reports the rejected ones as a gap;
 *  - sequence, below. Then packets++, bytes += payload, DATA.
 *
 * Timestamps are "comparable" when the previous accepted packet carried a
 * sample-count or free-running TSF and exactly samples_per_pkt samples
 * (rx->have_ts), and this packet carries the same TSF kind. Comparable
 * timestamps are always used to classify packets; cfg.gap_ts only selects
 * whether they also size gaps. samples_per_pkt is payload_bytes /
 * sample_size of the first data packet.
 *
 * With comparable timestamps, d = last_frac - tsf_frac when the timestamp
 * went backwards:
 *  - 0 < d < 16 * samples_per_pkt: late (reordered), late++, SKIP. Its
 *    payload is not delivered (the block has moved on), but it did arrive:
 *    lost_packets is decremented by one (not below 0), so lost_packets
 *    counts packets that never arrived;
 *  - d == 0 and the 4-bit count repeats the previous one: duplicates++,
 *    SKIP. Neither touches expected_count or last_frac;
 *  - d >= 16 * samples_per_pkt (more than the 4-bit count can reorder, e.g.
 *    a sender restart): a discontinuity. The packet is accepted with the
 *    4-bit gap, gap_unknown set and counted, and the tracker resyncs on it.
 *
 * Otherwise gap4 = (pkt_count - expected_count) & 0xF and, for timestamps
 * that went forwards, delta = tsf_frac - last_frac (64-bit) and
 * n = delta / samples_per_pkt. The timestamps size a gap when cfg.gap_ts,
 * they are comparable, this packet also has samples_per_pkt samples and
 * n >= 2: gap = n - 1.
 *  - gap4 == 0 and (not comparable or delta == samples_per_pkt): no gap;
 *  - gap4 == 0 and comparable timestamps disagree (a loss of 16k packets):
 *    gap = n - 1 when sized, else gap 0 with gap_unknown;
 *  - gap4 != 0: gap = n - 1 when sized; else gap = gap4, flagged gap_unknown
 *    when cfg.gap_ts asked for sizing (the true loss may be gap4 + 16k).
 *    With cfg.gap_ts off the 4-bit count is taken as is, not flagged.
 * Every gap or gap_unknown packet does gap_events++ and lost_packets += gap.
 *
 * Limitations: without comparable timestamps a repeated count is a loss of
 * 15 and a reordered packet is a loss. With them a late packet un-counts
 * one loss without checking that it fills a counted hole, so a stale
 * re-send can hide a real loss (gap_events is never decremented). A loss
 * of 16k packets without timestamps is undetectable. A gap_unknown packet
 * may report gap 0: callers must treat out->gap_unknown as a discontinuity.
 */

#ifndef DAQIRI_VRT_STREAM_H
#define DAQIRI_VRT_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dq_vrt_counters {
	uint64_t packets; /* accepted data packets (DQ_VRT_DATA) */
	uint64_t bytes;	  /* payload bytes of accepted data packets */
	uint64_t lost_packets, gap_events, gap_unknown;
	uint64_t foreign, context, malformed, duplicates, late;
};

struct dq_vrt_cfg {
	uint32_t sid, sid_mask;
	bool gap_ts;
	size_t sample_size; /* bytes per sample; 0 disables format checks */
};

struct dq_vrt_rx {
	struct dq_vrt_cfg cfg;
	bool sid_locked;
	uint32_t sid;
	int expected_count; /* -1 until the first data packet */
	bool have_ts;
	uint8_t tsf;
	uint64_t last_frac;
	uint32_t samples_per_pkt;
	bool class_checked;
	struct dq_vrt_counters c;
};

struct dq_vrt_data {
	const uint8_t *payload; /* points into the datagram */
	size_t len;
	uint32_t gap; /* packets lost before this one, clamped */
	bool gap_unknown;
};

#define DQ_VRT_DATA 0 /* out is valid */
#define DQ_VRT_SKIP 1 /* counted, nothing to copy */

void dq_vrt_rx_init(struct dq_vrt_rx *rx, const struct dq_vrt_cfg *cfg);

/*
 * Process one datagram (len bytes at dgram, starting at the VRT header).
 * Returns DQ_VRT_DATA (out filled in), DQ_VRT_SKIP (counted, out untouched)
 * or -EPROTO (payload not a multiple of sample_size, or packet class
 * incompatible with sample_size; nothing counted, out untouched).
 */
int dq_vrt_rx_packet(
		struct dq_vrt_rx *rx, const uint8_t *dgram, size_t len, struct dq_vrt_data *out);

#ifdef __cplusplus
}
#endif

#endif /* DAQIRI_VRT_STREAM_H */
