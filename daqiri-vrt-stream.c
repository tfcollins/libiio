// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * VITA 49.2 receive-side sequence tracker; see daqiri-vrt-stream.h.
 */

#include <errno.h>
#include <string.h>

#include "daqiri-vrt-prologue.h"
#include "daqiri-vrt-stream.h"

/* vrt49 packet class codes (vrt49 doc/source/reference/packet-classes.md). */
#define DQ_VRT_PCC_CI16 0x0010
#define DQ_VRT_PCC_REAL_INT16 0x0030

void dq_vrt_rx_init(struct dq_vrt_rx *rx, const struct dq_vrt_cfg *cfg)
{
	memset(rx, 0, sizeof(*rx));
	rx->cfg = *cfg;
	rx->expected_count = -1;
}

static bool dq_vrt_sid_match(const struct dq_vrt_rx *rx, const struct dq_vrt_prologue *p)
{
	if (rx->cfg.sid_mask)
		return p->has_sid && !((p->stream_id ^ rx->cfg.sid) & rx->cfg.sid_mask);

	return !rx->sid_locked || p->stream_id == rx->sid;
}

static int dq_vrt_check_format(struct dq_vrt_rx *rx, const struct dq_vrt_prologue *p)
{
	size_t ss = rx->cfg.sample_size;

	if (!ss)
		return 0;
	if (p->payload_bytes % ss)
		return -EPROTO;
	if (!rx->class_checked && p->has_class &&
			((p->packet_class == DQ_VRT_PCC_CI16 && ss != 4) ||
					(p->packet_class == DQ_VRT_PCC_REAL_INT16 && ss != 2)))
		return -EPROTO;

	rx->class_checked = true;
	return 0;
}

static bool dq_vrt_ts_kind(uint8_t tsf)
{
	return tsf == DQ_VRT_TSF_SAMPLE_COUNT || tsf == DQ_VRT_TSF_FREE_RUNNING;
}

int dq_vrt_rx_packet(
		struct dq_vrt_rx *rx, const uint8_t *dgram, size_t len, struct dq_vrt_data *out)
{
	struct dq_vrt_prologue p;
	size_t ss = rx->cfg.sample_size;
	uint64_t spp, delta, n, gap = 0;
	bool cmp, std, unknown = false;
	unsigned int gap4 = 0;
	int ret;

	if (dq_vrt_parse(dgram, len, &p)) {
		rx->c.malformed++;
		return DQ_VRT_SKIP;
	}
	if (p.type == DQ_VRT_IF_CONTEXT || p.type == DQ_VRT_EXT_CONTEXT) {
		rx->c.context++;
		return DQ_VRT_SKIP;
	}
	if (!dq_vrt_sid_match(rx, &p)) {
		rx->c.foreign++;
		return DQ_VRT_SKIP;
	}
	ret = dq_vrt_check_format(rx, &p);
	if (ret)
		return ret;

	if (!rx->cfg.sid_mask && !rx->sid_locked) {
		rx->sid_locked = true;
		rx->sid = p.stream_id;
	}
	if (rx->expected_count < 0)
		rx->samples_per_pkt = ss ? (uint32_t)(p.payload_bytes / ss) : 0;

	spp = rx->samples_per_pkt;
	std = spp && p.payload_bytes / ss == spp;
	/* have_ts implies spp > 0 and that the last packet had spp samples.
	 * With gap_ts off the timestamps are not used at all. */
	cmp = rx->cfg.gap_ts && rx->have_ts && p.tsf == rx->tsf;

	if (rx->expected_count >= 0)
		gap4 = (p.pkt_count - (unsigned int)rx->expected_count) & 0xF;

	if (cmp && p.tsf_frac < rx->last_frac) {
		if (rx->last_frac - p.tsf_frac < 16 * spp) {
			/* It did arrive, just too late to deliver. */
			rx->c.late++;
			if (rx->c.lost_packets)
				rx->c.lost_packets--;
			return DQ_VRT_SKIP;
		}
		/* Too far back to be reordered: resync on this packet. */
		gap = gap4;
		unknown = true;
	} else if (cmp && p.tsf_frac == rx->last_frac && gap4 == 15) {
		rx->c.duplicates++;
		return DQ_VRT_SKIP;
	} else if (cmp) {
		delta = p.tsf_frac - rx->last_frac;
		n = delta / spp;
		if (gap4 || delta != spp) {
			if (std && n >= 2) {
				gap = n - 1;
			} else {
				gap = gap4;
				unknown = true;
			}
		}
	} else if (gap4) {
		gap = gap4;
		unknown = rx->cfg.gap_ts;
	}

	if (gap || unknown) {
		rx->c.gap_events++;
		rx->c.lost_packets += gap;
		rx->c.gap_unknown += unknown;
	}

	rx->expected_count = (p.pkt_count + 1) & 0xF;
	rx->have_ts = dq_vrt_ts_kind(p.tsf) && std;
	rx->tsf = p.tsf;
	rx->last_frac = p.tsf_frac;
	rx->c.packets++;
	rx->c.bytes += p.payload_bytes;

	out->payload = dgram + p.prologue_bytes;
	out->len = p.payload_bytes;
	out->gap = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
	out->gap_unknown = unknown;
	return DQ_VRT_DATA;
}
