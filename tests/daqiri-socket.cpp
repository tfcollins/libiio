// SPDX-License-Identifier: LGPL-2.1-or-later
// Real DAQIRI socket engine; independent POSIX UDP peer, no mocked DAQIRI APIs.
#include <arpa/inet.h>
#include <daqiri/common.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

#include "daqiri-private.h"

using namespace std::chrono_literals;
using daqiri::Status;

#define CHECK(x) \
	do { \
		if (!(x)) { \
			std::cerr << "FAIL line " << __LINE__ << ": " #x << '\n'; \
			std::abort(); \
		} \
	} while (0)

struct Adapter {
	uintptr_t conn[2]{};
	uint16_t port[2]{};
	uint16_t queue[2]{};

	daqiri::BurstParams *rx[2]{};
	unsigned cursor[2]{};

	static int send(void *o, int lane, const void *p, size_t n)
	{
		auto &a = *static_cast<Adapter *>(o);
		auto b = daqiri::create_tx_burst_params();

		if (!b)
			return -EAGAIN;
		daqiri::set_header(b, a.port[lane], a.queue[lane], 1, 1);
		auto r = daqiri::get_tx_packet_burst(b);

		if (r != Status::SUCCESS) {
			daqiri::free_tx_metadata(b);
			return -EAGAIN;
		}
		std::memcpy(daqiri::get_packet_ptr(b, 0), p, n);
		daqiri::set_packet_lengths(b, 0, { int(n) });
		daqiri::set_connection_id(b, a.conn[lane]);
		r = daqiri::send_tx_burst(b);
		if (r != Status::SUCCESS && r != Status::NO_SPACE_AVAILABLE)
			daqiri::free_all_packets_and_burst_tx(b);
		return r == Status::SUCCESS ? 0 : -EAGAIN;
	}

	static int recv(void *o, int lane, void *p, size_t n)
	{
		auto &a = *static_cast<Adapter *>(o);
		auto &b = a.rx[lane];

		if (!b) {
			auto r = daqiri::get_rx_burst(&b, a.conn[lane], true);

			if (r != Status::SUCCESS)
				return 0;
			a.cursor[lane] = 0;
		}
		auto len = daqiri::get_packet_length(b, a.cursor[lane]);
		int result = len > n ? -EMSGSIZE : int(len);

		if (result >= 0)
			std::memcpy(p, daqiri::get_packet_ptr(b, a.cursor[lane]), len);
		if (++a.cursor[lane] >= daqiri::get_num_packets(b)) {
			daqiri::free_all_packets_and_burst_rx(b);
			b = nullptr;
		}
		return result;
	}

	~Adapter()
	{
		for (auto b : rx)
			if (b)
				daqiri::free_all_packets_and_burst_rx(b);
	}
};

// Peer encoding is intentionally independent of daqiri-wire.hpp.
static void put(unsigned char *p, uint64_t x, int n)
{
	for (int i = n - 1; i >= 0; --i) {
		p[i] = x & 255;
		x >>= 8;
	}
}

static uint64_t get(const unsigned char *p, int n)
{
	uint64_t x = 0;

	while (n--)
		x = x * 256 + *p++;
	return x;
}

struct Peer {
	int fd[2]{};
	std::atomic<bool> quit{ false };
	std::atomic<bool> allow{ false };
	std::atomic<unsigned> packets{ 0 };
	std::atomic<uint64_t> grant{ 0 };
	std::atomic<unsigned> starts{ 0 };
	std::atomic<unsigned> stops{ 0 };
	std::thread thread;
	int mode; // 0 RX, 1 TX, 2 malformed, 3 silent data, 4 bad HELLO

	Peer(int m)
	        : mode(m)
	{
		for (int i = 0; i < 2; ++i) {
			fd[i] = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
			CHECK(fd[i] >= 0);
			sockaddr_in a{};

			a.sin_family = AF_INET;
			a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			a.sin_port = htons(25102 + i * 10);
			CHECK(bind(fd[i], (sockaddr *)&a, sizeof(a)) == 0);
			a.sin_port = htons(25101 + i * 10);
			CHECK(connect(fd[i], (sockaddr *)&a, sizeof(a)) == 0);
		}
		thread = std::thread([this] { run(); });
	}

	void frame(int lane, int kind, uint64_t ep, uint64_t seq, uint64_t value,
	        const unsigned char *data = nullptr, size_t n = 0)
	{
		std::vector<unsigned char> p(40 + n);

		std::memcpy(p.data(), "DQI1", 4);
		p[4] = 1;
		p[5] = kind;
		put(p.data() + 8, ep, 8);
		put(p.data() + 16, seq, 8);
		put(p.data() + 24, value, 8);
		put(p.data() + 32, n, 4);
		if (n)
			std::memcpy(p.data() + 40, data, n);
		CHECK(::send(fd[lane], p.data(), p.size(), 0) == ssize_t(p.size()));
	}

	void run()
	{
		uint64_t ep = 0;
		uint64_t seq = 0;
		bool started = false;
		bool configured = false;
		size_t payload = 0;
		size_t max_block = 0;
		size_t extent = 0;
		size_t stride = 0;
		bool tx = false;
		unsigned char p[8192];
		unsigned char data[8192]{};

		while (!quit) {
			for (int lane = 1; lane >= 0; --lane) {
				auto n = ::recv(fd[lane], p, sizeof(p), 0);

				if (n < 40)
					continue;
				CHECK(std::memcmp(p, "DQI1", 4) == 0);
				CHECK(get(p + 32, 4) == uint64_t(n - 40));
				auto kind = p[5];

				ep = get(p + 8, 8);
				if (kind <= 3 || kind == 8) {
					if (kind == 1) {
						const auto *end = static_cast<const unsigned char *>(
						        std::memchr(p + 40, 0, size_t(n - 40)));
						CHECK(end && end + 21 <= p + n);
						const auto *limits = end + 1;

						payload = get(limits, 4);
						max_block = get(limits + 4, 4);
						stride = get(limits + 12, 4);
						tx = get(limits + 16, 4) != 0;
						CHECK(payload && payload <= sizeof(data) &&
						        stride && payload % stride == 0);
					}

					if (kind == 8) {
						CHECK(n == 44);
						extent = get(p + 40, 4);
						CHECK(tx ? extent == 0
						         : (extent && extent <= max_block &&
						                   extent % stride == 0));
						configured = true;
					}

					if (kind == 2) {
						CHECK(configured);
						started = true;
						++starts;
					}

					if (kind == 3) {
						started = false;
						++stops;
					}

					if (mode == 6 ||
					        ((mode == 5 || mode == 9 || mode == 10) &&
					                kind == 3) ||
					        ((mode == 7 || mode == 8 || mode == 9) &&
					                kind == 2) ||
					        (mode == 12 && kind == 8))
						continue;
					if (mode == 4 && kind == 1)
						p[40] ^= 1;
					if (mode == 11 && kind == 8)
						p[43] ^= 1;
					if (mode == 10 && kind == 2) {
						frame(1, 4, ep, get(p + 16, 8), 99);
						continue;
					}
					frame(1, 4, ep, get(p + 16, 8), kind, p + 40, n - 40);
				} else if (kind == 5)
					grant = get(p + 24, 8);
				else if (kind == 6) {
					CHECK(mode == 1);
					CHECK(get(p + 16, 8) == seq++);
					for (int j = 40; j < n; ++j)
						CHECK(p[j] ==
						        unsigned((packets.load() * 16 + j - 40) &
						                255));
					++packets;
				}
			}

			if (started && mode == 1 && allow)
				frame(1, 5, ep, 0, 3);

			if (started && mode == 0 && seq < grant) {
				const size_t slots = (extent + payload - 1) / payload;
				const size_t n =
				        std::min(payload, extent - (seq % slots) * payload);
				for (size_t i = 0; i < n; ++i)
					data[i] = (seq * payload + i) & 255;
				frame(0, 6, ep, seq++, 0, data, n);
				++packets;
			}

			if (started && mode == 2 && !packets) {
				frame(0, 6, ep, 0, 0, data, 1);
				++packets;
			}
			std::this_thread::sleep_for(1ms);
		}
	}

	~Peer()
	{
		quit = true;
		thread.join();
		for (int f : fd)
			close(f);
	}
};

static void scenario(const char *config, int mode, size_t extent = 40, bool multi = false)
{
	Peer peer(mode);

	CHECK(daqiri::daqiri_init(config) == Status::SUCCESS);
	{
		Adapter a;

		for (int i = 0; i < 2; ++i) {
			CHECK(daqiri::socket_get_server_conn_id(
			              "127.0.0.1", 25101 + i * 10, &a.conn[i]) == Status::SUCCESS);
			CHECK(daqiri::socket_get_port_queue(a.conn[i], &a.port[i], &a.queue[i]) ==
			        Status::SUCCESS);
		}

		dq_io io{ &a, Adapter::send, Adapter::recv };
		dq_profile p{};

		p.payload = 16;
		p.max_block = 40;
		p.max_blocks = multi ? 2 : 1;
		p.timeout_ms = 300;

		dq_stream *s = nullptr;
		int r = dq_stream_open(&io, &p, mode == 1, 4, "layout", 6, &s);

		if (mode == 4 || mode == 6) {
			CHECK(r == (mode == 4 ? -EPROTO : -ETIMEDOUT));
		} else {
			CHECK(r == 0);

			dq_block *b;
			void *data;

			CHECK(dq_block_create(s, extent, &b, &data) == 0);

			dq_block *second = nullptr;
			dq_block *extra;
			void *extra_data;

			if (multi) {
				CHECK(dq_block_create(s, extent - 4, &extra, &extra_data) ==
				        -EINVAL);
				CHECK(dq_block_create(s, extent, &second, &extra_data) == 0);
			}
			CHECK(dq_block_create(s, extent, &extra, &extra_data) == -ENOSPC);
			if (mode != 1) {
				CHECK(dq_block_create(s, extent - 4, &extra, &extra_data) ==
				        -EINVAL);
				CHECK(dq_block_enqueue(b, extent - 4, 0) == -EINVAL);
			}

			CHECK(dq_block_dequeue(b, 1) == -EPERM);
			CHECK(dq_block_enqueue(b, extent, 1) == -ENOSYS);
			CHECK(dq_block_enqueue(b, extent - 1, 0) == -EINVAL);

			for (size_t j = 0; j < extent; ++j)
				static_cast<unsigned char *>(data)[j] =
				        static_cast<unsigned char>(j);
			CHECK(dq_block_enqueue(b, extent, 0) == 0);
			CHECK(dq_block_enqueue(b, extent, 0) == -EPERM);
			CHECK(dq_block_dequeue(b, 1) == -EBUSY);
			if (second)
				CHECK(dq_block_enqueue(second, extent, 0) == 0);

			const auto begin = std::chrono::steady_clock::now();

			if (mode >= 7) {
				const int expected = mode == 8
				        ? -ECANCELED
				        : ((mode == 10 || mode == 11) ? -EPROTO : -ETIMEDOUT);
				if (mode == 8) {
					auto enable = std::async(std::launch::async,
					        [&] { return dq_stream_enable(s, 1); });
					while (!peer.starts &&
					        std::chrono::steady_clock::now() - begin < 1s)
						std::this_thread::sleep_for(1ms);
					CHECK(peer.starts > 0);
					dq_stream_cancel(s);
					CHECK(enable.get() == expected);
				} else
					CHECK(dq_stream_enable(s, 1) == expected);
				CHECK(std::chrono::steady_clock::now() - begin < 1s);
				CHECK(dq_block_dequeue(b, 0) == expected);
				if (mode <= 10) {
					CHECK(peer.starts > 0);
					CHECK(peer.stops > 0);
				} else
					CHECK(peer.starts == 0);
			} else {
				CHECK(dq_stream_enable(s, 1) == 0);
				CHECK(dq_stream_enable(s, 1) == -EBUSY);
				if (mode == 1) {
					std::this_thread::sleep_for(40ms);
					CHECK(peer.packets == 0);
					peer.allow = true;
				}
				if (mode <= 1) {
					CHECK(dq_block_dequeue(b, 0) == 0);
					CHECK(dq_block_dequeue(b, 0) == -EPERM);
					if (mode == 0) {
						for (size_t j = 0; j < extent; ++j)
							CHECK(static_cast<unsigned char *>(
							              data)[j] == j);
						if (second)
							CHECK(dq_block_dequeue(second, 0) == 0);
						std::this_thread::sleep_for(40ms);
						const size_t slots =
						        (extent + p.payload - 1) / p.payload;
						CHECK(peer.grant == (multi ? 2 : 1) * slots);
						CHECK(peer.packets == (multi ? 2 : 1) * slots);
						CHECK(dq_block_create(s, extent - 4, &extra,
						              &extra_data) == -EINVAL);
						CHECK(dq_block_enqueue(b, extent - 4, 0) ==
						        -EINVAL);
						CHECK(dq_block_enqueue(b, extent, 0) == 0);
						CHECK(dq_block_dequeue(b, 0) == 0);
						CHECK(peer.packets == (multi ? 3 : 2) * slots);
					} else {
						std::this_thread::sleep_for(20ms);
						CHECK(peer.packets == 3);
					}
				} else if (mode == 2)
					CHECK(dq_block_dequeue(b, 0) == -EPROTO);
				else if (mode == 3)
					CHECK(dq_block_dequeue(b, 0) == -ETIMEDOUT);
				else {
					auto waiter = std::async(std::launch::async,
					        [&] { return dq_block_dequeue(b, 0); });
					std::this_thread::sleep_for(20ms);
					dq_stream_cancel(s);
					CHECK(waiter.wait_for(100ms) == std::future_status::ready);
					CHECK(waiter.get() == -ECANCELED);
				}
			}

			dq_stream_cancel(s);
			CHECK(dq_block_enqueue(b, extent, 0) == -ECANCELED);
			dq_stream_close(s);
			dq_block_free(b); // block survives handle, storage freed after worker join
			dq_block_free(second);
		}
	}

	daqiri::shutdown();
	std::cout << "PASS mode " << mode << " extent " << extent << " multi " << multi << '\n';
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(dq_stream_enable(nullptr, 1) == -EINVAL);
	CHECK(dq_block_dequeue(nullptr, 0) == -EINVAL);
	for (int mode = 0; mode <= 12; ++mode)
		scenario(argv[1], mode);
	for (size_t extent : { 8u, 24u, 32u })
		scenario(argv[1], 0, extent);
	scenario(argv[1], 0, 24, true);
	std::cout
	        << "PASS real DAQIRI UDP socket RX/TX, negotiated extents, ambiguous START cleanup, credits, ownership, malformed, timeout, handshake rejection\n";
}
