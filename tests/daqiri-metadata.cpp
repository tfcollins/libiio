// SPDX-License-Identifier: LGPL-2.1-or-later
// NIC-free seam test: real production adapter + pinned upstream pool/ring.
#include <daqiri/common.h>

#include <cassert>
#include <cstdio>
#include <new>

#include "daqiri-wire.hpp"
#include "src/daqiri_pool.h"
namespace seam
{
using daqiri::Status;
using daqiri::BurstParams;
struct Slot {
	BurstParams burst{};
	unsigned char bytes[1514]{};
};
static daqiri::ObjectPool *tx, *rx;
static daqiri::Ring *tr[2], *rr[2];
static BurstParams *pending[2]{};
static unsigned slots[2]{};
static bool fail_alloc = false;
static BurstParams *alloc(daqiri::ObjectPool *p)
{
	void *v = nullptr;
	if (!p->get(&v))
		return nullptr;
	return &(new (v) Slot)->burst;
}
static BurstParams *create_tx_burst_params()
{
	return fail_alloc ? nullptr : alloc(tx);
}
static void set_header(BurstParams *b, int p, int q, int n, int s)
{
	b->hdr.hdr.port_id = p;
	b->hdr.hdr.q_id = q;
	b->hdr.hdr.num_pkts = n;
	b->hdr.hdr.num_segs = s;
}
static Status get_tx_packet_burst(BurstParams *b)
{
	auto q = b->hdr.hdr.q_id;
	if (slots[q] == 4096)
		return Status::NO_FREE_PACKET_BUFFERS;
	++slots[q];
	return Status::SUCCESS;
}
static void *get_segment_packet_ptr(BurstParams *b, int, int)
{
	return reinterpret_cast<Slot *>(b)->bytes;
}
static Status set_packet_lengths(BurstParams *, int, std::initializer_list<int>)
{
	return Status::SUCCESS;
}
static void free_tx_metadata(BurstParams *b)
{
	tx->put(b);
}
static void free_all_packets_and_burst_tx(BurstParams *b)
{
	--slots[b->hdr.hdr.q_id];
	tx->put(b);
}
static Status send_tx_burst(daqiri::EndpointId, int q, BurstParams *b)
{
	if (!pending[q]) {
		pending[q] = b;
		return Status::SUCCESS;
	} // worker stalled on full SQ
	if (tr[q]->enqueue(b))
		return Status::SUCCESS;
	seam::free_all_packets_and_burst_tx(b);
	return Status::NO_SPACE_AVAILABLE;
}
static Status get_rx_burst(BurstParams **b, int, int q)
{
	void *v = nullptr;
	if (!rr[q]->dequeue(&v))
		return Status::NOT_READY;
	*b = static_cast<BurstParams *>(v);
	return Status::SUCCESS;
}
static int get_num_packets(BurstParams *b)
{
	return b->hdr.hdr.num_pkts;
}
static size_t get_segment_packet_length(BurstParams *, int, int)
{
	return 43;
}
static void free_all_packets_and_burst_rx(BurstParams *b)
{
	rx->put(b);
}
}
#define DQ_PACKET_API seam
#include "../daqiri-transport.cpp"
static void frame(daqiri::BurstParams *b, int lane, int count = 1)
{
	seam::set_header(b, 0, lane, count, 1);
	auto p = static_cast<unsigned char *>(seam::get_segment_packet_ptr(b, 0, 0));
	dq::put(p + 12, 0x800, 2);
	p[14] = 0x45;
	p[23] = 17;
	dq::put(p + 16, 29, 2);
	dq::put(p + 34, 10000 + lane, 2);
	dq::put(p + 36, 10000 + lane, 2);
	dq::put(p + 38, 9, 2);
	p[42] = 0x5a;
}
static daqiri::NetworkConfig config()
{
	daqiri::NetworkConfig c{};
	c.common_.stream_type = daqiri::StreamType::RAW;
	c.common_.engine = c.common_.engine_type = daqiri::EngineType::IBVERBS;
	c.rx_meta_buffers_ = rx_metadata_min;
	c.tx_meta_buffers_ = tx_metadata_min;
	c.ifs_.resize(1);
	auto &i = c.ifs_[0];
	i.name_ = "test";
	i.rx_.queues_.resize(2);
	i.tx_.queues_.resize(2);
	for (int q = 0; q < 2; ++q) {
		i.rx_.queues_[q].poll_mode_ = i.tx_.queues_[q].poll_mode_ =
				daqiri::QueuePollMode::INDIRECT;
		int d = 0;
		for (auto common : { &i.rx_.queues_[q].common_, &i.tx_.queues_[q].common_ }) {
			auto name = std::to_string(q) + std::to_string(d++);
			common->id_ = q;
			common->batch_size_ = 1;
			common->mrs_ = { name };
			auto &m = c.mrs_[name];
			m.kind_ = daqiri::MemoryKind::HOST;
			m.buf_size_ = 1514;
			m.num_bufs_ = 4096;
		}
	}
	return c;
}
int main()
{
	dq_profile p{};
	std::strcpy(p.interface_name, "test");
	auto c = config();
	assert(valid_config(p, c));
	for (unsigned n : { 0U, 1U, 4096U, rx_metadata_min - 1 }) {
		c.rx_meta_buffers_ = n;
		assert(!valid_config(p, c));
	}
	c = config();
	for (unsigned n : { 0U, 1U, 4096U, tx_metadata_min - 1 }) {
		c.tx_meta_buffers_ = n;
		assert(!valid_config(p, c));
	}
	c = config();
	c.common_.engine = daqiri::EngineType::SOCKET;
	assert(!valid_config(p, c));
	void *v = nullptr;
	seam::tx = daqiri::ObjectPool::create("tx", tx_metadata_min - 1, sizeof(seam::Slot));
	seam::rx = daqiri::ObjectPool::create("rx", rx_metadata_min - 1, sizeof(seam::Slot));
	assert(seam::tx && seam::rx);
	for (int q = 0; q < 2; ++q) {
		seam::tr[q] = daqiri::Ring::create("tx", 4096, daqiri::RingMode::SPSC);
		seam::rr[q] = daqiri::Ring::create("rx", 2048, daqiri::RingMode::MPMC);
		assert(seam::tr[q] && seam::rr[q]);
		assert(seam::tr[q]->capacity() == 4095 && seam::rr[q]->capacity() == 2047);
	}
	dq_runtime r;
	r.profile.local_port = r.profile.peer_port = 10000;
	unsigned char byte = 0x5a, out = 0;
	// Reproduce old default metadata exhaustion through the PRODUCTION adapter:
	// 4095 data descriptors consume a 4096-configured (4095 usable) pool.
	auto safe = seam::tx;
	seam::tx = daqiri::ObjectPool::create("undersized", 4095, sizeof(seam::Slot));
	assert(seam::tx);
	for (unsigned j = 0; j < 4095; ++j)
		assert(send_packet(&r, 0, &byte, 1) == 0);
	assert(seam::tx->avail_count() == 0);
	assert(send_packet(&r, 1, &byte, 1) == -EAGAIN);
	seam::free_all_packets_and_burst_tx(seam::pending[0]);
	seam::pending[0] = nullptr;
	while (seam::tr[0]->dequeue(&v))
		seam::free_all_packets_and_burst_tx(static_cast<daqiri::BurstParams *>(v));
	assert(seam::tx->in_use_count() == 0);
	daqiri::ObjectPool::free(seam::tx);
	seam::tx = safe;
	// Saturated data TX (ring + worker pending) cannot starve control allocation.
	for (unsigned j = 0; j < 4096; ++j)
		assert(send_packet(&r, 0, &byte, 1) == 0);
	for (unsigned j = 0; j < 100; ++j)
		assert(send_packet(&r, 0, &byte, 1) == -EAGAIN);
	assert(send_packet(&r, 1, &byte, 1) == 0);
	for (unsigned j = 1; j < 4096; ++j)
		assert(send_packet(&r, 1, &byte, 1) == 0);
	assert(seam::tx->avail_count() ==
			1); // transient reservation remains even with BOTH lanes full
	assert(send_packet(&r, 1, &byte, 1) == -EAGAIN);
	assert(seam::tx->avail_count() == 1);
	// Impossible-under-invariant allocator fault is explicit, recoverable EAGAIN.
	seam::fail_alloc = true;
	assert(send_packet(&r, 1, &byte, 1) == -EAGAIN);
	seam::fail_alloc = false;
	auto b = seam::pending[1];
	seam::free_all_packets_and_burst_tx(b);
	seam::pending[1] = nullptr;
	assert(send_packet(&r, 1, &byte, 1) == 0);
	// RX worst case: each lane has a full ring, one adapter-held burst and
	// one worker-current burst, including a partial application burst.
	daqiri::BurstParams *current[2]{};
	for (int q = 0; q < 2; ++q) {
		r.rx[q] = seam::alloc(seam::rx);
		assert(r.rx[q]);
		frame(r.rx[q], q, 2);
		for (unsigned j = 0; j < 2047; ++j) {
			auto a = seam::alloc(seam::rx);
			assert(a);
			frame(a, q);
			assert(seam::rr[q]->enqueue(a));
		}
		current[q] = seam::alloc(seam::rx);
		assert(current[q]);
		frame(current[q], q);
	}
	assert(seam::rx->avail_count() == 0);
	assert(!seam::alloc(seam::rx));
	assert(recv_packet(&r, 1, &out, 1) == 1 &&
			out == byte); // held control advances at total exhaustion
	assert(recv_packet(&r, 1, &out, 1) == 1 && out == byte);
	assert(seam::rx->avail_count() == 1);
	assert(recv_packet(&r, 1, &out, 1) == 1 && out == byte); // queued control still advances
	assert(seam::rr[1]->enqueue(
			current[1])); // worker flush and reallocate under full data lane
	current[1] = seam::alloc(seam::rx);
	assert(current[1]);
	for (int q = 0; q < 2; ++q) {
		if (r.rx[q])
			seam::rx->put(r.rx[q]);
		seam::rx->put(current[q]);
		while (seam::rr[q]->dequeue(&v))
			seam::rx->put(v);
		seam::free_all_packets_and_burst_tx(seam::pending[q]);
		while (seam::tr[q]->dequeue(&v))
			seam::free_all_packets_and_burst_tx(static_cast<daqiri::BurstParams *>(v));
		daqiri::Ring::free(seam::rr[q]);
		daqiri::Ring::free(seam::tr[q]);
	}
	assert(seam::tx->in_use_count() == 0 && seam::rx->in_use_count() == 0);
	daqiri::ObjectPool::free(seam::tx);
	daqiri::ObjectPool::free(seam::rx);
	std::puts("PASS: metadata bounds, exhaustion, production adapter control progress (NIC-free allocation seam)");
}
