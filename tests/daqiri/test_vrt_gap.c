// SPDX-License-Identifier: MIT
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Unit test for the DAQIRI VRT sequence/gap tracker. Packets are built in
 * memory with the golden-vector layout from gen_golden.py: IF data with SID
 * 0x01000000, Class ID (OUI 0x000456, packet class 0x0010 = vrt49 CI16), TSI UTC and a
 * TSF sample count that advances 360 per packet.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daqiri-vrt-stream.h"
#include "test_framework.h"

#define SID 0x01000000u
#define OUI 0x000456u
#define SECS 1700000000u
#define SPP 360u
#define PROLOGUE 28u
#define BUF_SIZE (PROLOGUE + 4 * 4096)

struct pkt_opt {
	unsigned type; /* VRT packet type */
	uint32_t sid;
	int pclass;   /* -1: no Class ID */
	unsigned tsf; /* 0: no fractional timestamp */
};

#define PCC_CI16 0x0010
#define PCC_REAL_INT16 0x0030

static const struct pkt_opt def_opt = { 1, SID, PCC_CI16, 1 };

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

/* Builds one packet with nwords 32-bit payload words; returns its length. */
static size_t mk_pkt_opt(uint8_t *buf, const struct pkt_opt *o, unsigned pkt_count,
		uint64_t tsf_frac, unsigned nwords)
{
	size_t off = 4, i;
	bool has_sid = (o->type & 1) || o->type >= 4;

	if (has_sid) {
		put32(buf + off, o->sid);
		off += 4;
	}
	if (o->pclass >= 0) {
		put32(buf + off, OUI);
		put32(buf + off + 4, (uint32_t)o->pclass);
		off += 8;
	}
	put32(buf + off, SECS);
	off += 4;
	if (o->tsf) {
		put32(buf + off, (uint32_t)(tsf_frac >> 32));
		put32(buf + off + 4, (uint32_t)tsf_frac);
		off += 8;
	}
	for (i = 0; i < nwords; i++) {
		put32(buf + off, (uint32_t)i);
		off += 4;
	}
	put32(buf, ((uint32_t)o->type << 28) | ((uint32_t)(o->pclass >= 0) << 27) | (1u << 22) |
					((uint32_t)o->tsf << 20) | ((pkt_count & 0xFu) << 16) |
					(uint32_t)(off / 4));
	return off;
}

/* Default packet: CI16, nsamples samples of 4 bytes each. */
static size_t mk_pkt(uint8_t *buf, unsigned pkt_count, uint64_t tsf_frac, unsigned nsamples)
{
	return mk_pkt_opt(buf, &def_opt, pkt_count, tsf_frac, nsamples);
}

static void init_rx(struct dq_vrt_rx *rx, bool gap_ts, size_t sample_size)
{
	struct dq_vrt_cfg cfg = { 0 };

	cfg.gap_ts = gap_ts;
	cfg.sample_size = sample_size;
	dq_vrt_rx_init(rx, &cfg);
}

/* Feeds the packet with absolute index seq (count = seq % 16). */
static int feed_opt(struct dq_vrt_rx *rx, const struct pkt_opt *o, unsigned seq,
		struct dq_vrt_data *out)
{
	uint8_t buf[BUF_SIZE];
	size_t len = mk_pkt_opt(buf, o, seq, (uint64_t)seq * SPP, SPP);

	return dq_vrt_rx_packet(rx, buf, len, out);
}

static int feed(struct dq_vrt_rx *rx, unsigned seq, struct dq_vrt_data *out)
{
	return feed_opt(rx, &def_opt, seq, out);
}

/* Feeds seq[0..n-1]; returns how many of them returned DQ_VRT_DATA. */
static unsigned run(struct dq_vrt_rx *rx, const unsigned *seq, size_t n, struct dq_vrt_data *out)
{
	unsigned data = 0;
	size_t i;

	for (i = 0; i < n; i++)
		data += feed(rx, seq[i], out) == DQ_VRT_DATA;
	return data;
}

#define N(a) (sizeof(a) / sizeof((a)[0]))

TEST_FUNCTION(contiguous)
{
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	uint8_t buf[BUF_SIZE];
	unsigned i, ok = 0;
	size_t len;
	int ret;

	init_rx(&rx, true, 4);
	for (i = 0; i < 17; i++)
		ok += feed(&rx, i, &d) == DQ_VRT_DATA && d.gap == 0 && !d.gap_unknown &&
		      d.len == SPP * 4;
	TEST_ASSERT_EQ(ok, 17, "contiguous: every packet is gap-free DATA");
	TEST_ASSERT_EQ(rx.c.lost_packets, 0, "contiguous: lost");
	TEST_ASSERT_EQ(rx.c.gap_events, 0, "contiguous: gap_events");
	TEST_ASSERT_EQ(rx.c.packets, 17, "contiguous: packets");
	TEST_ASSERT_EQ(rx.c.bytes, 17 * SPP * 4, "contiguous: bytes");

	len = mk_pkt(buf, 17, 17 * SPP, SPP);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "payload: DATA");
	TEST_ASSERT(d.payload == buf + PROLOGUE, "payload: points past prologue");
	TEST_ASSERT_EQ(d.len, SPP * 4, "payload: length");
	TEST_ASSERT_EQ(rx.samples_per_pkt, SPP, "samples_per_pkt");
	TEST_ASSERT(rx.sid_locked && rx.sid == SID, "sid locked on first");
}

TEST_FUNCTION(drop_one)
{
	static const unsigned seq[] = { 0, 1, 2, 3, 4, 6 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	int ret;

	init_rx(&rx, true, 4);
	ret = run(&rx, seq, N(seq), &d);
	TEST_ASSERT_EQ(ret, N(seq), "drop one: all DATA");
	TEST_ASSERT_EQ(d.gap, 1, "drop one: out.gap");
	TEST_ASSERT(!d.gap_unknown, "drop one: sized");
	TEST_ASSERT_EQ(rx.c.lost_packets, 1, "drop one: lost");
	TEST_ASSERT_EQ(rx.c.gap_events, 1, "drop one: gap_events");
	ret = feed(&rx, 7, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "drop one: next DATA");
	TEST_ASSERT_EQ(d.gap, 0, "drop one: resynced");
}

TEST_FUNCTION(drop_three)
{
	static const unsigned seq[] = { 0, 1, 2, 3, 4, 8 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;

	init_rx(&rx, true, 4);
	run(&rx, seq, N(seq), &d);
	TEST_ASSERT_EQ(d.gap, 3, "drop three: out.gap");
	TEST_ASSERT_EQ(rx.c.lost_packets, 3, "drop three: lost");
	TEST_ASSERT_EQ(rx.c.gap_events, 1, "drop three: gap_events");

	/* Same loss without timestamp sizing: the 4-bit rule is accepted. */
	init_rx(&rx, false, 4);
	run(&rx, seq, N(seq), &d);
	TEST_ASSERT_EQ(rx.c.lost_packets, 3, "drop three no gap_ts: lost");
	TEST_ASSERT_EQ(rx.c.gap_unknown, 0, "drop three no gap_ts: known");
}

TEST_FUNCTION(wrap)
{
	/* Counts 14, 15, 1: count 0 is lost across the wrap. */
	static const unsigned seq[] = { 14, 15, 17 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	int ret;

	init_rx(&rx, true, 4);
	ret = run(&rx, seq, N(seq), &d);
	TEST_ASSERT_EQ(ret, 3, "wrap: all DATA");
	TEST_ASSERT_EQ(d.gap, 1, "wrap: out.gap");
	TEST_ASSERT_EQ(rx.c.lost_packets, 1, "wrap: lost");
}

TEST_FUNCTION(drop_many_ts)
{
	static const unsigned seq20[] = { 0, 1, 22 };
	static const unsigned seq16[] = { 0, 1, 18 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;

	init_rx(&rx, true, 4);
	run(&rx, seq20, N(seq20), &d);
	TEST_ASSERT_EQ(d.gap, 20, "drop 20: out.gap");
	TEST_ASSERT_EQ(rx.c.lost_packets, 20, "drop 20: lost");
	TEST_ASSERT_EQ(rx.c.gap_unknown, 0, "drop 20: gap_unknown");

	/* The 4-bit count sees no gap; the timestamps size it. */
	init_rx(&rx, true, 4);
	run(&rx, seq16, N(seq16), &d);
	TEST_ASSERT_EQ(d.gap, 16, "drop 16 gap_ts: out.gap");
	TEST_ASSERT_EQ(rx.c.lost_packets, 16, "drop 16 gap_ts: lost");
	TEST_ASSERT_EQ(rx.c.gap_events, 1, "drop 16 gap_ts: gap_events");
	TEST_ASSERT_EQ(rx.c.gap_unknown, 0, "drop 16 gap_ts: gap_unknown");
}

TEST_FUNCTION(drop_16_ambiguous)
{
	static const unsigned seq[] = { 0, 1, 18 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	int ret;

	init_rx(&rx, false, 4);
	ret = run(&rx, seq, N(seq), &d);
	TEST_ASSERT_EQ(ret, 3, "drop 16: DATA");
	TEST_ASSERT_EQ(rx.c.lost_packets, 0, "drop 16: lost");
	TEST_ASSERT_EQ(rx.c.gap_events, 1, "drop 16: gap_events");
	TEST_ASSERT_EQ(rx.c.gap_unknown, 1, "drop 16: gap_unknown");
	TEST_ASSERT(d.gap_unknown, "drop 16: out.gap_unknown");
	TEST_ASSERT_EQ(d.gap, 0, "drop 16: out.gap");
}

TEST_FUNCTION(huge_jump)
{
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	uint8_t buf[BUF_SIZE];
	size_t len;
	int ret;

	init_rx(&rx, true, 4);
	feed(&rx, 0, &d);
	feed(&rx, 1, &d);
	len = mk_pkt(buf, 2, UINT64_C(1) << 62, SPP);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "huge jump: DATA");
	TEST_ASSERT(d.gap == UINT32_MAX, "huge jump: out.gap clamped");
	TEST_ASSERT(rx.c.lost_packets == (UINT64_C(1) << 62) / SPP - 2,
			"huge jump: lost in 64 bits");
}

TEST_FUNCTION(duplicate)
{
	static const unsigned seq[] = { 0, 1 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	uint8_t buf[BUF_SIZE];
	size_t len;
	int ret;

	init_rx(&rx, true, 4);
	run(&rx, seq, N(seq), &d);
	ret = feed(&rx, 1, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "duplicate: SKIP");
	TEST_ASSERT_EQ(rx.c.duplicates, 1, "duplicate: duplicates");
	TEST_ASSERT_EQ(rx.c.foreign, 0, "duplicate: foreign untouched");
	ret = feed(&rx, 2, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "duplicate: next DATA");
	TEST_ASSERT_EQ(d.gap, 0, "duplicate: next gap-free");
	TEST_ASSERT_EQ(rx.c.lost_packets, 0, "duplicate: lost");
	TEST_ASSERT_EQ(rx.c.gap_events, 0, "duplicate: gap_events");
	TEST_ASSERT_EQ(rx.c.packets, 3, "duplicate: packets");

	/* Next count with a stalled timestamp is not a duplicate. */
	len = mk_pkt(buf, 3, 2 * SPP, SPP);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "stalled ts: DATA");
	TEST_ASSERT(d.gap_unknown && d.gap == 0, "stalled ts: unknown");
	TEST_ASSERT_EQ(rx.c.duplicates, 1, "stalled ts: not a duplicate");
}

TEST_FUNCTION(reorder)
{
	static const unsigned seq[] = { 0, 1, 2, 3, 4, 5, 7 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	int ret;

	init_rx(&rx, true, 4);
	run(&rx, seq, N(seq), &d);
	TEST_ASSERT_EQ(d.gap, 1, "reorder: 7 before 6 is a gap of 1");
	ret = feed(&rx, 6, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "reorder: late 6 SKIP");
	ret = feed(&rx, 8, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "reorder: 8 DATA");
	TEST_ASSERT_EQ(d.gap, 0, "reorder: 8 gap-free");
	TEST_ASSERT_EQ(rx.c.lost_packets, 0, "reorder: late 6 un-counts loss");
	TEST_ASSERT_EQ(rx.c.gap_events, 1, "reorder: gap_events");
	TEST_ASSERT_EQ(rx.c.late, 1, "reorder: late");
	TEST_ASSERT_EQ(rx.c.duplicates, 0, "reorder: duplicates");

	/* A stale re-send with nothing lost: late, lost stays at 0. */
	ret = feed(&rx, 3, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "stale: SKIP");
	TEST_ASSERT_EQ(rx.c.late, 2, "stale: late");
	TEST_ASSERT_EQ(rx.c.lost_packets, 0, "stale: lost floors at 0");
}

TEST_FUNCTION(backward_step)
{
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	uint8_t buf[BUF_SIZE];
	unsigned i;
	size_t len;
	int ret;

	/* Sender restart: count and timestamp go back to 0 after 20 packets,
	 * more than 15 packets behind, so it resyncs instead of being late. */
	init_rx(&rx, true, 4);
	for (i = 0; i < 20; i++)
		feed(&rx, i, &d);
	len = mk_pkt(buf, 0, 0, SPP);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "restart: DATA");
	TEST_ASSERT(d.gap_unknown, "restart: out.gap_unknown");
	TEST_ASSERT_EQ(d.gap, 12, "restart: 4-bit gap");
	TEST_ASSERT_EQ(rx.c.late, 0, "restart: not late");
	TEST_ASSERT_EQ(rx.c.gap_events, 1, "restart: gap_events");
	TEST_ASSERT_EQ(rx.c.gap_unknown, 1, "restart: gap_unknown");
	ret = feed(&rx, 1, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "restart: next DATA");
	TEST_ASSERT_EQ(d.gap, 0, "restart: resynced");
	TEST_ASSERT_EQ(rx.c.gap_events, 1, "restart: no second event");
}

TEST_FUNCTION(no_timestamps)
{
	static const struct pkt_opt nots = { 1, SID, PCC_CI16, 0 };
	static const unsigned seq[] = { 0, 1, 2, 3, 4, 8 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	size_t i;
	int ret;

	/* gap_ts requested but unusable: 4-bit size, flagged unknown. */
	init_rx(&rx, true, 4);
	for (i = 0; i < N(seq); i++)
		feed_opt(&rx, &nots, seq[i], &d);
	TEST_ASSERT_EQ(d.gap, 3, "no ts: 4-bit gap");
	TEST_ASSERT(d.gap_unknown, "no ts: out.gap_unknown");
	TEST_ASSERT_EQ(rx.c.lost_packets, 3, "no ts: lost");
	TEST_ASSERT_EQ(rx.c.gap_unknown, 1, "no ts: gap_unknown");

	/* A repeated count cannot be told from a loss of 15 without ts. */
	init_rx(&rx, true, 4);
	feed_opt(&rx, &nots, 0, &d);
	feed_opt(&rx, &nots, 1, &d);
	ret = feed_opt(&rx, &nots, 1, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "no ts repeat: DATA");
	TEST_ASSERT_EQ(d.gap, 15, "no ts repeat: gap 15");
	TEST_ASSERT_EQ(rx.c.lost_packets, 15, "no ts repeat: lost");
	TEST_ASSERT_EQ(rx.c.duplicates, 0, "no ts repeat: duplicates");
}

TEST_FUNCTION(odd_size)
{
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	uint8_t buf[BUF_SIZE];
	size_t len;
	int ret;

	/* 360, 360, 100 samples, then the next in sequence (frac 820): the
	 * short packet must not make the timestamp delta look like a loss. */
	init_rx(&rx, true, 4);
	feed(&rx, 0, &d);
	feed(&rx, 1, &d);
	len = mk_pkt(buf, 2, 720, 100);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "odd: short packet DATA");
	TEST_ASSERT_EQ(d.len, 400, "odd: short packet copied");
	len = mk_pkt(buf, 3, 820, SPP);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "odd: next DATA");
	TEST_ASSERT_EQ(rx.c.gap_events, 0, "odd: no false gap");
	TEST_ASSERT_EQ(rx.samples_per_pkt, SPP, "odd: spp fixed by first");

	/* Two lost after the short packet: timestamps would say 1, the
	 * 4-bit rule says 2 and is used, flagged unknown. */
	init_rx(&rx, true, 4);
	feed(&rx, 0, &d);
	feed(&rx, 1, &d);
	len = mk_pkt(buf, 2, 720, 100);
	dq_vrt_rx_packet(&rx, buf, len, &d);
	len = mk_pkt(buf, 5, 720 + 100 + 2 * SPP, SPP);
	dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(d.gap, 2, "odd then gap: 4-bit size");
	TEST_ASSERT(d.gap_unknown, "odd then gap: unknown");
	TEST_ASSERT_EQ(rx.c.lost_packets, 2, "odd then gap: lost");

	/* The short packet itself follows a gap: 4-bit rule for it too. */
	init_rx(&rx, true, 4);
	feed(&rx, 0, &d);
	feed(&rx, 1, &d);
	len = mk_pkt(buf, 3, 3 * SPP, 100);
	dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(d.gap, 1, "gap into odd: 4-bit size");
	TEST_ASSERT(d.gap_unknown, "gap into odd: unknown");
}

TEST_FUNCTION(sid_filter)
{
	struct pkt_opt o = def_opt;
	struct dq_vrt_cfg cfg = { 0 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	int ret;

	cfg.sid = SID;
	cfg.sid_mask = 0xFFF00000;
	cfg.gap_ts = true;
	cfg.sample_size = 4;
	dq_vrt_rx_init(&rx, &cfg);
	o.sid = 0x02100000;
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "mask: SKIP");
	TEST_ASSERT_EQ(rx.c.foreign, 1, "mask: foreign");
	o.type = 0; /* no Stream ID cannot match a mask */
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "mask nosid");
	TEST_ASSERT_EQ(rx.c.foreign, 2, "mask nosid: foreign");
	o.type = 1;
	o.sid = SID | 0x5; /* masked-out bits are ignored */
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "mask: match");
	TEST_ASSERT_EQ(rx.c.packets, 1, "mask: packets");

	/* A packet without Stream ID never matches, even a zero sid. */
	cfg.sid = 0;
	dq_vrt_rx_init(&rx, &cfg);
	o.type = 0;
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "mask sid 0 nosid: SKIP");
	o.type = 1;

	/* No mask: lock on the first IF data packet. */
	init_rx(&rx, true, 4);
	o.sid = 0x01000007;
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "lock: first");
	TEST_ASSERT(rx.sid_locked && rx.sid == 0x01000007, "lock: sid");
	ret = feed(&rx, 1, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "lock: other SID");
	TEST_ASSERT_EQ(rx.c.foreign, 1, "lock: foreign");
	ret = feed_opt(&rx, &o, 1, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "lock: same");
	TEST_ASSERT_EQ(d.gap, 0, "lock: foreign does not disturb seq");
}

TEST_FUNCTION(context)
{
	static const struct pkt_opt ctx = { 4, SID, PCC_CI16, 1 };
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	int ret;

	init_rx(&rx, true, 4);
	ret = feed_opt(&rx, &ctx, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "context first: SKIP");
	TEST_ASSERT(!rx.sid_locked, "context does not lock SID");
	feed(&rx, 0, &d);
	ret = feed_opt(&rx, &ctx, 5, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "context: SKIP");
	ret = feed(&rx, 1, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "context: next DATA");
	TEST_ASSERT_EQ(d.gap, 0, "context: no gap");
	TEST_ASSERT_EQ(rx.c.context, 2, "context: counted");
	TEST_ASSERT_EQ(rx.c.packets, 2, "context: packets");
}

TEST_FUNCTION(malformed)
{
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	uint8_t tmp[BUF_SIZE], *buf;
	size_t len = mk_pkt(tmp, 0, 0, SPP);
	int ret;

	init_rx(&rx, true, 4);
	/* Exact-size heap copy one word short: ASan catches an over-read. */
	buf = malloc(len - 4);
	TEST_ASSERT_PTR_NOT_NULL(buf, "malloc");
	if (!buf)
		return;
	memcpy(buf, tmp, len - 4);
	ret = dq_vrt_rx_packet(&rx, buf, len - 4, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "truncated: SKIP");
	ret = dq_vrt_rx_packet(&rx, buf, 3, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_SKIP, "3 bytes: SKIP");
	TEST_ASSERT_EQ(rx.c.malformed, 2, "truncated: malformed");
	TEST_ASSERT_EQ(rx.c.packets, 0, "truncated: packets");
	TEST_ASSERT(!rx.sid_locked, "truncated: no lock");
	free(buf);
}

TEST_FUNCTION(eproto)
{
	struct pkt_opt o = def_opt;
	struct dq_vrt_rx rx;
	struct dq_vrt_data d;
	uint8_t buf[BUF_SIZE];
	size_t len;
	int ret;

	/* VRT payloads are whole words, so use 8-byte samples and a
	 * 12-byte payload; no Class ID so only the size check applies. */
	o.pclass = -1;
	init_rx(&rx, true, 8);
	len = mk_pkt_opt(buf, &o, 0, 0, 4);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "16 bytes / 8: DATA");
	len = mk_pkt_opt(buf, &o, 1, 2, 3);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, -EPROTO, "12 bytes / 8: -EPROTO");
	TEST_ASSERT_EQ(rx.c.packets, 1, "-EPROTO: not counted");
	/* The rejected packet left the sequence state alone. */
	len = mk_pkt_opt(buf, &o, 2, 4, 4);
	ret = dq_vrt_rx_packet(&rx, buf, len, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "after -EPROTO: DATA");
	TEST_ASSERT_EQ(d.gap, 1, "after -EPROTO: gap");

	/* Packet class checks on the first data packet. */
	init_rx(&rx, true, 2);
	ret = feed(&rx, 0, &d);
	TEST_ASSERT_EQ(ret, -EPROTO, "CI16 with 2: -EPROTO");
	TEST_ASSERT(!rx.class_checked, "CI16 with 2: not checked");
	init_rx(&rx, true, 8);
	ret = feed(&rx, 0, &d);
	TEST_ASSERT_EQ(ret, -EPROTO, "CI16 with 8: -EPROTO");
	o = def_opt;
	o.pclass = PCC_REAL_INT16;
	init_rx(&rx, true, 4);
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, -EPROTO, "REAL_INT16 with 4: -EPROTO");
	init_rx(&rx, true, 2);
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "REAL_INT16 with 2: DATA");
	TEST_ASSERT(rx.class_checked, "REAL_INT16 with 2: checked");

	/* Other classes, 0 and the old 0x0001 included, are unchecked. */
	o.pclass = 0;
	init_rx(&rx, true, 4);
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "class 0 with 4: DATA");
	o.pclass = 0x0001;
	init_rx(&rx, true, 2);
	ret = feed_opt(&rx, &o, 0, &d);
	TEST_ASSERT_EQ(ret, DQ_VRT_DATA, "class 0x0001 with 2: DATA");
}

int main(void)
{
	RUN_TEST(contiguous);
	RUN_TEST(drop_one);
	RUN_TEST(drop_three);
	RUN_TEST(wrap);
	RUN_TEST(drop_many_ts);
	RUN_TEST(drop_16_ambiguous);
	RUN_TEST(huge_jump);
	RUN_TEST(duplicate);
	RUN_TEST(reorder);
	RUN_TEST(backward_step);
	RUN_TEST(no_timestamps);
	RUN_TEST(odd_size);
	RUN_TEST(sid_filter);
	RUN_TEST(context);
	RUN_TEST(malformed);
	RUN_TEST(eproto);
	TEST_SUMMARY();
}
