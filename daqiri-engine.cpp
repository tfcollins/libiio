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

#include <daqiri/daqiri.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "daqiri-private.h"

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
