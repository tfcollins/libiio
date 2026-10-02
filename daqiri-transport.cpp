// SPDX-License-Identifier: LGPL-2.1-or-later
#include <arpa/inet.h>
#include <daqiri/common.h>

#include <cerrno>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "daqiri-private.h"
#include "daqiri-wire.hpp"
#ifndef DAQIRI_PROFILE_DIR
#define DAQIRI_PROFILE_DIR "/etc/libiio/daqiri"
#endif
// Private compile-time packet/allocation seam for NIC-free adapter tests only.
// Normal builds always call the real DAQIRI API; no runtime fallback exists.
#ifndef DQ_PACKET_API
#define DQ_PACKET_API daqiri
#endif
using daqiri::Status;
// Pinned 993363d: RX rings hold 2047; TX rings hold 4095 descriptors.
// See tests/daqiri-metadata.md for the complete ownership proof.
static constexpr unsigned rx_metadata_min = 2 * (2047 + 1 + 1) + 1;
static constexpr unsigned tx_metadata_min = 2 * (4095 + 1) + 1 + 1;
static std::mutex broker;
static bool owned = false;
struct dq_runtime {
	dq_profile profile{};
	daqiri::NetworkConfig config{};
	std::mutex tx_lock, rx_lock; // one producer/consumer and bounded application holdings
	int port = -1;
	daqiri::EndpointId endpoints[2]{};
	daqiri::BurstParams *rx[2]{};
	int cursor[2]{};
	uint32_t local = 0, peer = 0;
};
static int status(Status s)
{
	switch (s) {
	case Status::SUCCESS:
		return 0;
	case Status::NOT_READY:
	case Status::NO_FREE_BURST_BUFFERS:
	case Status::NO_FREE_PACKET_BUFFERS:
	case Status::NO_SPACE_AVAILABLE:
		return -EAGAIN;
	case Status::INVALID_PARAMETER:
	case Status::NULL_PTR:
		return -EINVAL;
	case Status::NOT_SUPPORTED:
		return -ENOSYS;
	case Status::RESOURCE_IN_USE:
	case Status::ALREADY_EXISTS:
		return -EBUSY;
	default:
		return -EIO;
	}
}
extern "C" int dq_profile_load(const char *name, dq_profile *out)
{
	try {
		if (!out || !name || !*name || std::strlen(name) > 64)
			return -EINVAL;
		for (auto p = name; *p; ++p)
			if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
					    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
				return -EINVAL;
		std::ifstream f(std::string(DAQIRI_PROFILE_DIR) + "/" + name + ".conf");
		if (!f)
			return -ENOENT;
		std::map<std::string, std::string> values;
		std::string line;
		while (std::getline(f, line)) {
			if (line.size() > 1024)
				return -EINVAL;
			if (line.empty() || line[0] == '#')
				continue;
			auto eq = line.find('=');
			if (eq == std::string::npos || !eq || eq == line.size() - 1)
				return -EINVAL;
			if (!values.emplace(line.substr(0, eq), line.substr(eq + 1)).second)
				return -EINVAL;
		}
		dq_profile p{};
		auto text = [&](const char *key, char *dst, size_t n) {
			auto i = values.find(key);
			if (i == values.end() || i->second.size() >= n)
				throw std::invalid_argument(key);
			std::strcpy(dst, i->second.c_str());
			values.erase(i);
		};
		auto number = [&](const char *key, uint32_t max) {
			auto i = values.find(key);
			if (i == values.end())
				throw std::invalid_argument(key);
			uint64_t v = 0;
			for (char c : i->second) {
				if (c < '0' || c > '9')
					throw std::invalid_argument(key);
				v = v * 10 + c - '0';
				if (v > max)
					throw std::invalid_argument(key);
			}
			if (!v)
				throw std::invalid_argument(key);
			values.erase(i);
			return static_cast<uint32_t>(v);
		};
		if (values["version"] != "1" || values["mode"] != "raw-ibverbs-cpu" ||
				values["trusted_network"] != "yes")
			return -ENOTSUP;
		values.erase("version");
		values.erase("mode");
		values.erase("trusted_network");
		text("management", p.management, sizeof(p.management));
		text("yaml", p.yaml, sizeof(p.yaml));
		text("interface", p.interface_name, sizeof(p.interface_name));
		text("device", p.device, sizeof(p.device));
		text("local_ip", p.local_ip, sizeof(p.local_ip));
		text("peer_ip", p.peer_ip, sizeof(p.peer_ip));
		text("peer_mac", p.peer_mac, sizeof(p.peer_mac));
		p.management_port = number("management_port", 65535);
		p.local_port = number("local_port", 65534);
		p.peer_port = number("peer_port", 65534);
		p.payload = number("payload", 1400);
		p.max_block = number("max_block", 64 * 1024 * 1024);
		p.max_blocks = number("max_blocks", 64);
		p.timeout_ms = number("timeout_ms", 60000);
		in_addr a;
		if (!values.empty() || p.yaml[0] != '/' ||
				inet_pton(AF_INET, p.management, &a) != 1 ||
				inet_pton(AF_INET, p.local_ip, &a) != 1 ||
				inet_pton(AF_INET, p.peer_ip, &a) != 1 ||
				uint64_t(p.max_block) * p.max_blocks > 256 * 1024 * 1024)
			return -EINVAL;
		*out = p;
		return 0;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EINVAL;
	}
}
static bool valid_config(const dq_profile &p, const daqiri::NetworkConfig &c)
{
	if (c.common_.stream_type != daqiri::StreamType::RAW ||
			c.common_.engine != daqiri::EngineType::IBVERBS || c.ifs_.size() != 1)
		return false;
	if (c.common_.engine_type != daqiri::EngineType::IBVERBS)
		return false;
	const auto &i = c.ifs_[0];
	if (i.name_ != p.interface_name || !i.rx_.reorder_configs_.empty() ||
			i.rx_.queues_.size() != 2 || i.tx_.queues_.size() != 2)
		return false;
	std::set<std::string> pools;
	size_t total = 0;
	for (const auto &kv : c.mrs_) {
		const auto &m = kv.second;
		if (m.kind_ != daqiri::MemoryKind::HOST || m.buf_size_ < 1514 ||
				m.buf_size_ > 65536 || !m.num_bufs_ || m.num_bufs_ > 4096)
			return false;
		total += m.buf_size_ * m.num_bufs_;
	}
	if (total > 256 * 1024 * 1024 || c.mrs_.size() != 4 ||
			c.tx_meta_buffers_ < tx_metadata_min ||
			c.rx_meta_buffers_ < rx_metadata_min || c.tx_meta_buffers_ > 16384 ||
			c.rx_meta_buffers_ > 16384)
		return false;
	for (unsigned q = 0; q < 2; ++q) {
		const auto &rx = i.rx_.queues_[q];
		const auto &tx = i.tx_.queues_[q];
		if (rx.poll_mode_ != daqiri::QueuePollMode::INDIRECT ||
				tx.poll_mode_ != daqiri::QueuePollMode::INDIRECT)
			return false;
		for (auto common : { &rx.common_, &tx.common_ }) {
			if (common->id_ != int(q) || common->mrs_.size() != 1 ||
					common->split_boundary_ || !common->offloads_.empty() ||
					common->batch_size_ < 1 || common->batch_size_ > 64)
				return false;
			if (!c.mrs_.count(common->mrs_[0]) || !pools.insert(common->mrs_[0]).second)
				return false;
		}
	}
	return true;
}
struct TxBurstGuard {
	daqiri::BurstParams *burst;
	bool packets = false;
	~TxBurstGuard() noexcept
	{
		if (!burst)
			return;
		try {
			if (packets)
				DQ_PACKET_API::free_all_packets_and_burst_tx(burst);
			else
				DQ_PACKET_API::free_tx_metadata(burst);
		} catch (...) {
		}
	}
};
static int send_packet(void *opaque, int lane, const void *src, size_t size)
{
	try {
		auto &r = *static_cast<dq_runtime *>(opaque);
		if (lane < 0 || lane > 1 || size > 1440)
			return -EINVAL;
		std::lock_guard<std::mutex> lock(r.tx_lock);
		auto b = DQ_PACKET_API::create_tx_burst_params();
		if (!b)
			return -EAGAIN;
		TxBurstGuard guard{ b };
		DQ_PACKET_API::set_header(b, r.port, lane, 1, 1);
		auto st = DQ_PACKET_API::get_tx_packet_burst(b);
		if (st != Status::SUCCESS)
			return status(st);
		guard.packets = true;
		auto dst = DQ_PACKET_API::get_segment_packet_ptr(b, 0, 0);
		if (!dst)
			return -EIO;
		std::memcpy(dst, src, size);
		st = DQ_PACKET_API::set_packet_lengths(b, 0, { static_cast<int>(size) });
		if (st == Status::SUCCESS) {
			st = DQ_PACKET_API::send_tx_burst(r.endpoints[lane], lane, b);
			// Both of these consume metadata AND packets, including queue-full drops.
			if (st == Status::SUCCESS || st == Status::NO_SPACE_AVAILABLE) {
				guard.burst = nullptr;
				return status(st);
			}
		}
		return status(st);
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
}
static int recv_packet(void *opaque, int lane, void *dst, size_t capacity)
{
	try {
		auto &r = *static_cast<dq_runtime *>(opaque);
		if (lane < 0 || lane > 1)
			return -EINVAL;
		std::lock_guard<std::mutex> lock(r.rx_lock);
		auto &b = r.rx[lane];
		if (!b) {
			auto st = DQ_PACKET_API::get_rx_burst(&b, r.port, lane);
			if (st != Status::SUCCESS) {
				b = nullptr;
				int e = status(st);
				return e == -EAGAIN ? 0 : e;
			}
			r.cursor[lane] = 0;
		}
		int result = -EPROTO;
		auto count = DQ_PACKET_API::get_num_packets(b);
		if (count > 0 && count <= 64 && b->hdr.hdr.num_segs == 1) {
			auto p = static_cast<unsigned char *>(DQ_PACKET_API::get_segment_packet_ptr(
					b, 0, r.cursor[lane]));
			size_t n = DQ_PACKET_API::get_segment_packet_length(b, 0, r.cursor[lane]);
			// Baseline forbids VLAN, IPv4 options and fragments. Hardware steering is
			// not trusted as validation: bind both IPv4 addresses and UDP ports here.
			if (p && n >= 42 && dq::get(p + 12, 2) == 0x800 && p[14] == 0x45 &&
					p[23] == 17 && !(dq::get(p + 20, 2) & 0x3fff) &&
					std::memcmp(p + 26, &r.peer, 4) == 0 &&
					std::memcmp(p + 30, &r.local, 4) == 0 &&
					dq::get(p + 34, 2) ==
							static_cast<uint64_t>(r.profile.peer_port +
									      lane) &&
					dq::get(p + 36, 2) ==
							static_cast<uint64_t>(r.profile.local_port +
									      lane)) {
				size_t udp = dq::get(p + 38, 2), ip = dq::get(p + 16, 2);
				if (udp >= 8 && ip == udp + 20 && 14 + ip <= n &&
						udp - 8 <= capacity) {
					std::memcpy(dst, p + 42, udp - 8);
					result = static_cast<int>(udp - 8);
				}
			}
		}
		if (++r.cursor[lane] >= count || result < 0) {
			DQ_PACKET_API::free_all_packets_and_burst_rx(b);
			b = nullptr;
		}
		return result;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
}
extern "C" int dq_runtime_open(const dq_profile *p, dq_runtime **out, dq_io *io)
{
	bool initialized = false;
	std::unique_lock<std::mutex> lock(broker, std::defer_lock);
	try {
		lock.lock();
		if (!p || !out || !io)
			return -EINVAL;
		if (owned)
			return -EBUSY;
		std::unique_ptr<dq_runtime> r(new dq_runtime);
		r->profile = *p;
		int e = status(daqiri::parse_network_config_from_yaml_file(p->yaml, r->config));
		if (e)
			return e;
		if (!valid_config(*p, r->config))
			return -ENOTSUP;
		e = status(daqiri::daqiri_init(r->config));
		if (e)
			return e; // never shutdown somebody else's engine
		initialized = true;
		r->port = daqiri::get_port_id(p->interface_name);
		if (r->port < 0) {
			daqiri::shutdown();
			return -ENODEV;
		}
		inet_pton(AF_INET, p->local_ip, &r->local);
		inet_pton(AF_INET, p->peer_ip, &r->peer);
		for (int lane = 0; lane < 2; ++lane) {
			daqiri::RawUdpEndpointConfig ep;
			ep.name_ = "libiio-dqi1-" + std::to_string(lane);
			ep.interface_ = p->interface_name;
			ep.dst_mac_ = p->peer_mac;
			ep.src_ipv4_ = p->local_ip;
			ep.dst_ipv4_ = p->peer_ip;
			ep.src_port_ = p->local_port + lane;
			ep.dst_port_ = p->peer_port + lane;
			ep.mtu_ = 1514;
			e = status(daqiri::add_endpoint(ep, &r->endpoints[lane]));
			if (e) {
				daqiri::shutdown();
				return e;
			}
		}
		owned = true;
		*io = { r.get(), send_packet, recv_packet };
		*out = r.release();
		return 0;
	} catch (const std::bad_alloc &) {
		if (initialized)
			daqiri::shutdown();
		return -ENOMEM;
	} catch (...) {
		if (initialized)
			daqiri::shutdown();
		return -EIO;
	}
}
extern "C" void dq_runtime_close(dq_runtime *r)
{
	if (!r)
		return;
	try {
		std::lock_guard<std::mutex> lock(broker);
		for (auto &b : r->rx)
			if (b) {
				DQ_PACKET_API::free_all_packets_and_burst_rx(b);
				b = nullptr;
			}
		// Engine owns all TX buffers. Never release its DMA allocations on a drain
		// timeout: shutdown is the engine's destruction/quiescence boundary.
		daqiri::wait_for_tx_idle(r->profile.timeout_ms);
		daqiri::shutdown();
		delete r;
		owned = false;
	} catch (...) {
		// Quarantine the runtime and the broker lease when quiescence is uncertain.
	}
}
