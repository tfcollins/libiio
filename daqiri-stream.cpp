// SPDX-License-Identifier: LGPL-2.1-or-later
#include <sys/random.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "daqiri-wire.hpp"

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct dq_block;

struct dq_stream {
	dq_io io{};
	dq_profile profile{};
	bool tx = false;
	bool started = false;
	size_t stride = 0;
	size_t blocks = 0;
	size_t rx_extent = 0;
	uint64_t epoch = 0;
	uint64_t seq = 0;
	uint64_t grant = 0;
	uint64_t remote_grant = 0;

	std::atomic<bool> cancel{ false };
	std::mutex mutex;
	std::mutex lifecycle;
	std::condition_variable changed;
	std::thread worker;
	std::deque<dq_block *> pending;
	int error = 0;
};

struct dq_block {
	std::shared_ptr<dq_stream> stream;
	std::vector<unsigned char> bytes;
	size_t used = 0;
	size_t offset = 0;

	enum { APP, QUEUED, DONE } state = APP;
};

/* The outer handle is owned by the backend; blocks keep the state alive. */
struct StreamHandle {
	std::shared_ptr<dq_stream> state;
};

static std::shared_ptr<dq_stream> state(dq_stream *s)
{
	return reinterpret_cast<StreamHandle *>(s)->state;
}

static void fail(dq_stream &s, int error)
{
	if (!s.error)
		s.error = error;
	s.changed.notify_all();
}

static int command(dq_stream &s, unsigned kind, uint64_t id, const void *body = nullptr,
        size_t size = 0, bool stopping = false)
{
	unsigned char tx[DQ_MAX_PACKET];
	unsigned char rx[DQ_MAX_PACKET];
	auto length = dq::encode(tx, kind, s.epoch, id, 0, body, size);
	auto deadline = Clock::now() + std::chrono::milliseconds(s.profile.timeout_ms);
	auto next = Clock::now();

	while (Clock::now() < deadline) {
		if (s.cancel && !stopping)
			return -ECANCELED;
		if (Clock::now() >= next) {
			int r = s.io.send(s.io.opaque, 1, tx, length);

			if (r && r != -EAGAIN)
				return r;
			next = Clock::now() + 20ms;
		}

		int n = s.io.recv(s.io.opaque, 1, rx, sizeof(rx));

		if (n < 0)
			return n;
		if (n) {
			dq::Packet p;

			if (!dq::decode(rx, n, p))
				return -EPROTO;
			if (p.epoch != s.epoch)
				continue;
			if (p.kind == dq::ERROR)
				return -EIO;
			if (p.kind == dq::ACK && p.seq == id) {
				if (p.value != kind || p.size != size ||
				        (size && std::memcmp(p.payload, body, size)))
					return -EPROTO;
				return 0;
			}
		}
		std::this_thread::sleep_for(1ms);
	}
	return -ETIMEDOUT;
}

static void run(dq_stream &s) noexcept
{
	try {
		unsigned char packet[DQ_MAX_PACKET];
		auto progress = Clock::now();
		auto refresh = Clock::now();

		while (!s.cancel) {
			std::unique_lock<std::mutex> lock(s.mutex);

			if (s.error)
				break;

			/* Control first, one datagram per iteration: data cannot starve STOP/credit. */
			int n = s.io.recv(s.io.opaque, 1, packet, sizeof(packet));

			if (n < 0) {
				fail(s, n);
				break;
			}
			if (n) {
				dq::Packet p;

				if (!dq::decode(packet, n, p)) {
					fail(s, -EPROTO);
					break;
				}
				if (p.epoch == s.epoch) {
					uint64_t window = uint64_t(s.profile.max_blocks) *
					        ((s.profile.max_block + s.profile.payload - 1) /
					                s.profile.payload);
					if (p.kind == dq::ERROR) {
						fail(s, -EIO);
						break;
					}
					if (p.kind == dq::CREDIT) {
						if (p.size || p.seq || !s.tx ||
						        p.value > s.seq + window) {
							fail(s, -EPROTO);
							break;
						}
						s.remote_grant = std::max(s.remote_grant, p.value);
					} else if (p.kind != dq::ACK) {
						fail(s, -EPROTO);
						break;
					}
				}
			}

			if (!s.tx && Clock::now() >= refresh) {
				auto len = dq::encode(packet, dq::CREDIT, s.epoch, 0, s.grant);
				int r = s.io.send(s.io.opaque, 1, packet, len);

				if (r && r != -EAGAIN) {
					fail(s, r);
					break;
				}
				refresh = Clock::now() + 20ms;
			}

			if (s.pending.empty())
				progress = Clock::now();
			if (s.tx) {
				if (!s.pending.empty() && s.seq < s.remote_grant) {
					auto b = s.pending.front();
					size_t bytes = std::min<size_t>(
					        s.profile.payload, b->used - b->offset);
					auto len = dq::encode(packet, dq::DATA, s.epoch, s.seq, 0,
					        b->bytes.data() + b->offset, bytes);
					int r = s.io.send(s.io.opaque, 0, packet, len);

					if (!r) {
						++s.seq;
						b->offset += bytes;
						progress = Clock::now();
					} else if (r != -EAGAIN) {
						fail(s, r);
						break;
					}
				}
			} else {
				n = s.io.recv(s.io.opaque, 0, packet, sizeof(packet));
				if (n < 0) {
					fail(s, n);
					break;
				}
				if (n) {
					dq::Packet p;

					if (!dq::decode(packet, n, p)) {
						fail(s, -EPROTO);
						break;
					}
					if (p.epoch == s.epoch) {
						if (p.kind != dq::DATA || p.value ||
						        p.seq >= s.grant) {
							fail(s, -EPROTO);
							break;
						}
						if (p.seq > s.seq) {
							fail(s, -EIO);
							break;
						} // strict window=1, no hidden holes
						if (p.seq == s.seq) {
							if (s.pending.empty()) {
								fail(s, -EPROTO);
								break;
							}
							auto b = s.pending.front();
							auto wanted =
							        std::min<size_t>(s.profile.payload,
							                b->used - b->offset);
							if (p.size != wanted || p.size % s.stride) {
								fail(s, -EPROTO);
								break;
							}
							std::memcpy(b->bytes.data() + b->offset,
							        p.payload, p.size);
							b->offset += p.size;
							++s.seq;
							progress = Clock::now();
						}
					}
				}
			}

			if (!s.pending.empty()) {
				auto b = s.pending.front();

				if (b->offset == b->used) {
					b->state = dq_block::DONE;
					s.pending.pop_front();
					s.changed.notify_all();
				} else if (Clock::now() - progress >
				        std::chrono::milliseconds(s.profile.timeout_ms)) {
					fail(s, -ETIMEDOUT);
					break;
				}
			}

			lock.unlock();
			std::this_thread::sleep_for(1ms);
		}

		/* Even failed streams terminate the epoch; no new data is admitted. */
		command(s, dq::STOP, 3, nullptr, 0, true);
	} catch (const std::bad_alloc &) {
		std::lock_guard<std::mutex> l(s.mutex);

		fail(s, -ENOMEM);
	} catch (...) {
		std::lock_guard<std::mutex> l(s.mutex);

		fail(s, -EIO);
	}
}

extern "C" int dq_stream_open(const dq_io *io, const dq_profile *p, int tx, size_t stride,
        const void *description, size_t size, dq_stream **out)
{
	try {
		if (!p || !io || !io->send || !io->recv || !out || !description || !size ||
		        size > 1024 || !stride || !p->payload ||
		        p->payload > DQ_MAX_PACKET - DQ_HEADER || p->payload % stride ||
		        !p->max_blocks || p->max_blocks > 64 || !p->max_block ||
		        p->max_block > 64 * 1024 * 1024 || p->max_block < stride ||
		        uint64_t(p->max_block) * p->max_blocks > 256 * 1024 * 1024 ||
		        !p->timeout_ms || p->timeout_ms > 60000)
			return -EINVAL;

		auto s = std::make_shared<dq_stream>();

		s->io = *io;
		s->profile = *p;
		s->stride = stride;
		s->tx = tx != 0;
		if (getrandom(&s->epoch, sizeof(s->epoch), 0) != sizeof(s->epoch) || !s->epoch)
			return -EIO;

		/* HELLO body is the complete canonical layout plus all mandatory limits.
		 * Peer must echo exactly; an ACK with a different contract fails closed. */
		unsigned char body[1200];
		const char cap[] = DQ_CAPABILITY;
		size_t capsize = sizeof(cap);

		std::memcpy(body, cap, capsize);
		dq::put(body + capsize, p->payload, 4);
		dq::put(body + capsize + 4, p->max_block, 4);
		dq::put(body + capsize + 8, p->max_blocks, 4);
		dq::put(body + capsize + 12, stride, 4);
		dq::put(body + capsize + 16, tx != 0, 4);
		std::memcpy(body + capsize + 20, description, size);

		int r = command(*s, dq::HELLO, 1, body, capsize + 20 + size);

		if (r)
			return r;

		auto h = new StreamHandle{ s };
		*out = reinterpret_cast<dq_stream *>(h);
		return 0;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
}

extern "C" void dq_stream_cancel(dq_stream *h)
{
	if (!h)
		return;

	try {
		auto s = state(h);

		s->cancel = true;
		std::lock_guard<std::mutex> l(s->mutex);

		s->changed.notify_all();
	} catch (...) {
	}
}

static void stop(const std::shared_ptr<dq_stream> &s)
{
	s->cancel = true;
	{
		std::lock_guard<std::mutex> l(s->mutex);

		s->changed.notify_all();
	}
	std::lock_guard<std::mutex> l(s->lifecycle);

	if (s->worker.joinable())
		s->worker.join();
}

extern "C" void dq_stream_close(dq_stream *h)
{
	if (!h)
		return;

	try {
		stop(state(h));
		delete reinterpret_cast<StreamHandle *>(h);
	} catch (...) { /* Retain state rather than destroy a possibly live worker. */
	}
}

extern "C" int dq_stream_enable(dq_stream *h, int enable)
{
	try {
		if (!h)
			return -EINVAL;
		auto s = state(h);

		if (!enable) {
			stop(s);
			return 0;
		}
		std::lock_guard<std::mutex> lifecycle(s->lifecycle);
		std::lock_guard<std::mutex> l(s->mutex);

		if (s->cancel)
			return -ECANCELED;
		if (s->started)
			return -EBUSY;
		if (s->error)
			return s->error;
		if (!s->blocks)
			return -EINVAL;

		/* A slot grant alone cannot describe a short block-tail packet. Freeze the
		 * actual RX extent at first allocation, and have the peer echo it before
		 * START. TX uses zero: its sender supplies packet lengths. */
		unsigned char extent[4];

		dq::put(extent, s->rx_extent, 4);
		int r = command(*s, dq::CONFIG, 4, extent, sizeof(extent));

		if (r) {
			fail(*s, r);
			return r;
		}

		r = command(*s, dq::START, 2);
		if (r) {
			/* START may have taken effect even when its ACK is lost or cancel wins.
			 * STOP has its own bounded deadline and deliberately ignores cancel.
			 * Preserve the original error even if STOP itself fails. */
			fail(*s, r);
			command(*s, dq::STOP, 3, nullptr, 0, true);
			return r;
		}

		try {
			s->worker = std::thread([s] { run(*s); });
		} catch (...) {
			s->cancel = true;
			fail(*s, -EIO);
			command(*s, dq::STOP, 3, nullptr, 0, true);
			return -EIO;
		}

		s->started = true;
		return 0;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
}

extern "C" int dq_block_create(dq_stream *h, size_t size, dq_block **out, void **data)
{
	try {
		if (!h || !out || !data)
			return -EINVAL;
		auto s = state(h);
		std::lock_guard<std::mutex> l(s->mutex);

		if (s->cancel)
			return -ECANCELED;
		if (!size || size > s->profile.max_block || size % s->stride)
			return -EINVAL;
		if (!s->tx && s->rx_extent && size != s->rx_extent)
			return -EINVAL;
		if (s->blocks >= s->profile.max_blocks)
			return -ENOSPC;

		std::unique_ptr<dq_block> b(new dq_block);

		b->stream = s;
		b->bytes.resize(size);
		*data = b->bytes.data();
		*out = b.release();
		++s->blocks;
		if (!s->tx)
			s->rx_extent = size;
		return 0;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
}

extern "C" void dq_block_free(dq_block *b)
{
	if (!b)
		return;

	try {
		auto s = b->stream;
		// Destruction terminates the stream, joining all users before storage release.
		stop(s);
		{
			std::lock_guard<std::mutex> l(s->mutex);

			s->pending.erase(std::remove(s->pending.begin(), s->pending.end(), b),
			        s->pending.end());
			--s->blocks;
		}
		delete b;
	} catch (...) { /* Quarantine storage if joining failed. */
	}
}

extern "C" int dq_block_enqueue(dq_block *b, size_t used, int cyclic)
{
	try {
		if (!b)
			return -EINVAL;
		auto s = b->stream;
		std::lock_guard<std::mutex> l(s->mutex);

		if (cyclic)
			return -ENOSYS;
		if (s->cancel)
			return -ECANCELED;
		if (s->error)
			return s->error;
		if (b->state != dq_block::APP)
			return -EPERM;
		if (!used || used > b->bytes.size() || used % s->stride)
			return -EINVAL;
		if (!s->tx && used != s->rx_extent)
			return -EINVAL;

		uint64_t slots = (used + s->profile.payload - 1) / s->profile.payload;

		if (s->grant > UINT64_MAX - slots - 65536 || s->seq > UINT64_MAX - slots - 65536)
			return -EOVERFLOW;

		s->pending.push_back(b);
		b->used = used;
		b->offset = 0;
		b->state = dq_block::QUEUED;
		if (!s->tx)
			s->grant += slots;
		return 0;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
}

extern "C" int dq_block_dequeue(dq_block *b, int nonblock)
{
	try {
		if (!b)
			return -EINVAL;
		auto s = b->stream;
		std::unique_lock<std::mutex> l(s->mutex);

		if (b->state == dq_block::APP)
			return -EPERM;

		auto ready = [&] { return s->cancel || s->error || b->state == dq_block::DONE; };

		if (nonblock && !ready())
			return -EBUSY;
		if (!nonblock &&
		        !s->changed.wait_for(
		                l, std::chrono::milliseconds(s->profile.timeout_ms), ready)) {
			fail(*s, -ETIMEDOUT);
			s->cancel = true;
		}
		int r = s->cancel ? (s->error ? s->error : -ECANCELED) : s->error;

		if (!r)
			b->state = dq_block::APP;
		return r;
	} catch (...) {
		return -EIO;
	}
}
