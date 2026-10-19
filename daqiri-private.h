/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Shared between the C side of the DAQIRI backend module and the C++ shim in
 * daqiri-engine.cpp, which is the only file that includes DAQIRI headers.
 */

#ifndef __DAQIRI_PRIVATE_H__
#define __DAQIRI_PRIVATE_H__

#include <iio/iio-backend.h>
#include <iio/iiod-client.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest host part accepted in a URI: an RFC 1035 name, or a bracketed IPv6
 * literal with scope id, plus ":65535". */
#define DQ_HOST_MAX (255 + sizeof("[]:65535"))

enum dq_framing { DQ_FRAMING_VRT = 0, DQ_FRAMING_IIOD = 1 };
enum dq_mem { DQ_MEM_HOST = 0, DQ_MEM_PINNED = 1, DQ_MEM_DEVICE = 2 };

struct dq_opts {
	char yaml[4096];
	char iface[64];
	int rxq;
	enum dq_framing framing;
	enum dq_mem mem;
	int gpu;
	bool lossy;
	bool swap;
	bool gap_ts;
	uint32_t sid, sid_mask;
	uint16_t data_port;
	unsigned poll_us;
};

/* <list> is the text after the first ',' of the URI, or NULL. Applies the
 * defaults first, then every key=value pair; -EINVAL on anything unknown. */
int dq_opts_parse(struct dq_opts *o, const char *list, const struct iio_context_params *p);

/* One TCP connection to iiod. A pointer to it is what this module hands to
 * iiod_client_new() as its struct iiod_client_pdata. */
struct dq_mgmt {
	int fd;
	int cancel_fd;
	const struct iio_context_params *params;
};

int dq_mgmt_connect(struct dq_mgmt *m, const char *host, const char *port, int timeout_ms);
void dq_mgmt_close(struct dq_mgmt *m);
extern const struct iiod_client_ops dq_mgmt_ops;

struct iio_context_pdata {
	struct dq_mgmt mgmt;
	struct iiod_client *client;
	struct dq_opts opts;
	/* Resolved host and management port; framing=iiod buffers connect here */
	char host[DQ_HOST_MAX];
	char port[sizeof("65535")];
};

struct dq_vrt_buffer;
struct dq_vrt_block;

struct iio_buffer_pdata {
	struct iio_context_pdata *ctx;
	const struct iio_device *dev;
	unsigned int idx;
	enum dq_framing framing;

	/* framing=iiod: a TCP connection of its own, as ip: does */
	struct {
		struct dq_mgmt io;
		struct iiod_client *client;
		struct iiod_client_buffer_pdata *pdata;
	} iiod;

	struct dq_vrt_buffer *vrt;
};

/*
 * iiod_block is what iiod_client_create_block() returned: iiod-client's own
 * struct iio_block_pdata, a different type behind the same tag. It is only
 * ever handed back to iiod-client, never dereferenced here.
 */
struct iio_block_pdata {
	struct iio_buffer_pdata *buf;
	struct iio_block_pdata *iiod_block;
	struct dq_vrt_block *vrt;
};

/* C++ side (daqiri-engine.cpp). Never throw; errors are negative errnos. */
int dq_yaml_validate(const char *path, char *err, size_t errlen);
const char *dq_engine_version(void);
const char *dq_yaml_engine_kind(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* __DAQIRI_PRIVATE_H__ */
