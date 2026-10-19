// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * The only translation unit of the DAQIRI backend that includes DAQIRI (C++)
 * headers. Every entry point is extern "C", catches everything and reports
 * failures as negative errno codes.
 */

#include <cuda_runtime_api.h>
#include <daqiri/daqiri.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <utility>

#include "daqiri-private.h"

/* The process-wide engine; never freed (its lifetime is the process's) */
struct dq_engine {
	std::string path; /* realpath of the YAML */
	daqiri::NetworkConfig cfg;
	unsigned int refs = 0;
	std::set<std::pair<int, int> > rxqs; /* (port, queue) with an owner */
};

struct dq_rxq {
	dq_engine *e;
	int port, queue;
	unsigned int l2_offset;
};

namespace
{

int status_to_errno(daqiri::Status st)
{
	switch (st) {
	case daqiri::Status::SUCCESS:
		return 0;
	case daqiri::Status::NOT_READY:
		return -EAGAIN;
	case daqiri::Status::NO_FREE_BURST_BUFFERS:
	case daqiri::Status::NO_FREE_PACKET_BUFFERS:
		return -ENOBUFS;
	case daqiri::Status::INVALID_PARAMETER:
		return -EINVAL;
	case daqiri::Status::NOT_SUPPORTED:
		return -ENOTSUP;
	case daqiri::Status::RESOURCE_IN_USE:
		return -EBUSY;
	default:
		return -EIO;
	}
}

void set_err(char *err, size_t errlen, const char *msg)
{
	if (err && errlen)
		std::snprintf(err, errlen, "%s", msg);
}

/* Keep DAQIRI's INFO chatter out of a parse-only check; warnings and errors
 * still get through. The previous level is restored on exit. */
class quiet_info {
    public:
	quiet_info()
		: saved(daqiri::get_log_level())
	{
		if (saved < daqiri::LogSeverity::WARN)
			daqiri::set_log_level(daqiri::LogSeverity::WARN);
	}
	~quiet_info()
	{
		daqiri::set_log_level(saved);
	}
	quiet_info(const quiet_info &) = delete;
	quiet_info &operator=(const quiet_info &) = delete;

    private:
	daqiri::LogSeverity saved;
};

/* Guards g_engine and its rxqs set */
std::mutex g_lock;
dq_engine *g_engine;

/* DAQIRI logs its init at INFO; keep that quiet unless asked for. */
bool dq_debug()
{
	const char *env = std::getenv("IIO_DAQIRI_DEBUG");

	return env && env[0] && std::strcmp(env, "0");
}

/* Pin this module: the core dlclose()s it with the last context, but the
 * engine and its threads live until exit. */
int pin_module(char *err, size_t errlen)
{
	static bool pinned;
	Dl_info info;

	if (pinned)
		return 0;

	if (!dladdr((void *)&dq_engine_acquire, &info) || !info.dli_fname ||
			!dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE)) {
		const char *msg = dlerror();

		set_err(err, errlen, msg ? msg : "cannot pin the module");
		return -ELIBACC;
	}

	pinned = true;
	return 0;
}

const daqiri::InterfaceConfig *find_iface(const dq_engine *e, int port)
{
	for (const auto &ifc : e->cfg.ifs_)
		if (daqiri::get_port_id(ifc.name_.empty() ? ifc.address_ : ifc.name_) == port)
			return &ifc;

	return nullptr;
}

/* Parse only: never initialises the engine. */
int parse(const char *path, daqiri::NetworkConfig &cfg, char *err, size_t errlen)
{
	if (!path || !path[0]) {
		set_err(err, errlen, "no file given");
		return -EINVAL;
	}

	/* Report an unreadable file ourselves rather than through DAQIRI's log */
	std::FILE *f = std::fopen(path, "r");
	if (!f) {
		int ret = -errno;

		set_err(err, errlen, std::strerror(errno));
		return ret;
	}
	std::fclose(f);

	quiet_info quiet;
	daqiri::Status st = daqiri::parse_network_config_from_yaml_file(path, cfg);
	if (st != daqiri::Status::SUCCESS) {
		set_err(err, errlen, "rejected by the DAQIRI parser");
		return status_to_errno(st);
	}

	return 0;
}

} // namespace

extern "C" int dq_yaml_validate(const char *path, char *err, size_t errlen)
{
	try {
		daqiri::NetworkConfig cfg{};

		return parse(path, cfg, err, errlen);
	} catch (const std::bad_alloc &) {
		set_err(err, errlen, "out of memory");
		return -ENOMEM;
	} catch (const std::exception &e) {
		set_err(err, errlen, e.what());
		return -EINVAL;
	} catch (...) {
		set_err(err, errlen, "unknown exception");
		return -EIO;
	}
}

extern "C" const char *dq_engine_version(void)
{
	return daqiri::version_string();
}

extern "C" const char *dq_yaml_engine_kind(const char *path)
{
	try {
		daqiri::NetworkConfig cfg{};
		const daqiri::CommonConfig &c = cfg.common_;

		if (parse(path, cfg, nullptr, 0))
			return "unknown";

		/* Before daqiri_init() resolves it, engine_type may still be UNKNOWN */
		daqiri::EngineType engine = daqiri::is_explicit_engine_type(c.engine_type)
							    ? c.engine_type
							    : c.engine;

		switch (c.stream_type) {
		case daqiri::StreamType::SOCKET:
			if (c.protocol == daqiri::SocketProtocol::ROCE ||
					engine == daqiri::EngineType::RDMA)
				return "roce";
			return "socket";
		case daqiri::StreamType::RAW:
			if (engine == daqiri::EngineType::DPDK)
				return "dpdk";
			if (engine == daqiri::EngineType::IBVERBS)
				return "ibverbs";
			return "unknown";
		default:
			return "unknown";
		}
	} catch (...) {
		return "unknown";
	}
}

extern "C" int dq_engine_acquire(const char *yaml, struct dq_engine **out, char *err, size_t errlen)
{
	try {
		char real[PATH_MAX];
		daqiri::Status st;
		int ret;

		if (!yaml || !yaml[0]) {
			set_err(err, errlen, "no YAML given");
			return -EINVAL;
		}
		if (!realpath(yaml, real)) {
			ret = -errno;
			set_err(err, errlen, std::strerror(errno));
			return ret;
		}

		std::lock_guard<std::mutex> lock(g_lock);

		if (g_engine) {
			if (g_engine->path != real) {
				std::string msg = "this process runs DAQIRI from " + g_engine->path;

				set_err(err, errlen, msg.c_str());
				return -EBUSY;
			}
			g_engine->refs++;
			*out = g_engine;
			return 0;
		}

		ret = pin_module(err, errlen);
		if (ret)
			return ret;

		auto e = std::make_unique<dq_engine>();
		e->path = real;

		ret = parse(real, e->cfg, err, errlen);
		if (ret)
			return ret;

		if (dq_debug()) {
			st = daqiri::daqiri_init(e->cfg);
		} else {
			quiet_info quiet;
			st = daqiri::daqiri_init(e->cfg);
		}
		if (st != daqiri::Status::SUCCESS) {
			set_err(err, errlen, "daqiri_init failed");
			return status_to_errno(st);
		}

		e->refs = 1;
		g_engine = e.release();
		*out = g_engine;
		return 0;
	} catch (const std::bad_alloc &) {
		set_err(err, errlen, "out of memory");
		return -ENOMEM;
	} catch (const std::exception &e) {
		set_err(err, errlen, e.what());
		return -EIO;
	} catch (...) {
		set_err(err, errlen, "unknown exception");
		return -EIO;
	}
}

extern "C" void dq_engine_release(struct dq_engine *e)
{
	std::lock_guard<std::mutex> lock(g_lock);

	/* No shutdown: the engine lives as long as the process */
	if (e && e->refs)
		e->refs--;
}

extern "C" int dq_engine_get_port(struct dq_engine *e, const char *iface, int *port_id)
{
	try {
		std::string key;
		int port;

		if (iface && iface[0]) {
			key = iface;
		} else if (!e->cfg.ifs_.empty()) {
			const daqiri::InterfaceConfig &ifc = e->cfg.ifs_.front();

			key = ifc.name_.empty() ? ifc.address_ : ifc.name_;
		}

		port = daqiri::get_port_id(key);
		if (port < 0)
			return -EINVAL;

		*port_id = port;
		return 0;
	} catch (...) {
		return -EIO;
	}
}

extern "C" int dq_rxq_open(struct dq_engine *e, int port_id, int queue_id, struct dq_rxq **out)
{
	try {
		const daqiri::CommonConfig &c = e->cfg.common_;
		const daqiri::InterfaceConfig *ifc;
		bool found = false;

		std::lock_guard<std::mutex> lock(g_lock);

		ifc = find_iface(e, port_id);
		if (!ifc)
			return -EINVAL;
		for (const auto &q : ifc->rx_.queues_)
			found |= q.common_.id_ == queue_id;
		if (!found)
			return -EINVAL;

		/* One datagram per packet, VRT header first (l2_offset 0). The
		 * raw engines need L2/L3/L4 validation and a non-zero
		 * l2_offset; they come with a later phase. */
		if (c.stream_type != daqiri::StreamType::SOCKET ||
				c.protocol != daqiri::SocketProtocol::UDP)
			return -ENOTSUP;

		if (!e->rxqs.emplace(port_id, queue_id).second)
			return -EBUSY;

		*out = new dq_rxq{ e, port_id, queue_id, 0 };
		return 0;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	} catch (...) {
		return -EIO;
	}
}

extern "C" void dq_rxq_close(struct dq_rxq *q)
{
	if (!q)
		return;

	{
		std::lock_guard<std::mutex> lock(g_lock);

		q->e->rxqs.erase(std::make_pair(q->port, q->queue));
	}
	delete q;
}

extern "C" int dq_rxq_poll(struct dq_rxq *q, struct dq_burst *b)
{
	try {
		daqiri::BurstParams *burst = nullptr;
		daqiri::Status st = daqiri::get_rx_burst(&burst, q->port, q->queue);

		if (st == daqiri::Status::NOT_READY || st == daqiri::Status::NULL_PTR ||
				(st == daqiri::Status::SUCCESS && !burst))
			return -EAGAIN;
		if (st != daqiri::Status::SUCCESS)
			return status_to_errno(st);

		b->handle = burst;
		b->nb_pkts = (unsigned)daqiri::get_num_packets(burst);
		b->l2_offset = q->l2_offset;
		return 0;
	} catch (...) {
		return -EIO;
	}
}

extern "C" unsigned dq_burst_pkts(const struct dq_burst *b, struct dq_pkt *out, unsigned max)
{
	auto *burst = static_cast<daqiri::BurstParams *>(b->handle);
	unsigned i, n = b->nb_pkts < max ? b->nb_pkts : max;

	try {
		for (i = 0; i < n; i++) {
			out[i].data = static_cast<const uint8_t *>(
					daqiri::get_packet_ptr(burst, (int)i));
			out[i].len = daqiri::get_packet_length(burst, (int)i);
		}
	} catch (...) {
		return 0;
	}

	return n;
}

extern "C" void dq_burst_free(struct dq_burst *b)
{
	try {
		if (b->handle)
			daqiri::free_all_packets_and_burst_rx(
					static_cast<daqiri::BurstParams *>(b->handle));
	} catch (...) {
	}
	b->handle = nullptr;
	b->nb_pkts = 0;
}

#define DQ_PAGE 4096

extern "C" int dq_mem_alloc(enum dq_mem k, int gpu, size_t size, void **ptr)
{
	size_t len = (size + DQ_PAGE - 1) & ~(size_t)(DQ_PAGE - 1);
	cudaError_t ce;
	void *p = nullptr;

	(void)gpu; /* portable pinned memory is usable from every device */

	switch (k) {
	case DQ_MEM_HOST:
		p = aligned_alloc(DQ_PAGE, len ? len : DQ_PAGE);
		if (!p)
			return -ENOMEM;
		/* Advice only: fewer TLB misses where THP is enabled */
		madvise(p, len, MADV_HUGEPAGE);
		break;
	case DQ_MEM_PINNED:
		ce = cudaHostAlloc(&p, size ? size : 1, cudaHostAllocPortable);
		if (ce != cudaSuccess) {
			cudaGetLastError();
			return ce == cudaErrorMemoryAllocation ? -ENOMEM : -ENOTSUP;
		}
		break;
	default:
		/* mem=device needs a raw engine with header-data split */
		return -ENOTSUP;
	}

	*ptr = p;
	return 0;
}

extern "C" void dq_mem_free(enum dq_mem k, void *ptr, size_t size)
{
	(void)size;

	switch (k) {
	case DQ_MEM_HOST:
		free(ptr);
		break;
	case DQ_MEM_PINNED:
		if (ptr)
			cudaFreeHost(ptr);
		break;
	default:
		break;
	}
}
