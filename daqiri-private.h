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
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "daqiri-asm.h"

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

struct dq_engine;
struct dq_rxq;
struct dq_vrt_buffer;
struct dq_vrt_block;

struct iio_context_pdata {
	struct dq_mgmt mgmt;
	struct iiod_client *client;
	struct dq_opts opts;
	/* Resolved host and management port; framing=iiod buffers connect here */
	char host[DQ_HOST_MAX];
	char port[sizeof("65535")];
	/* What the YAML selects ("socket", ...), "none" without a YAML */
	const char *engine_kind;

	/* framing=vrt: protects engine and vrt_bufs */
	pthread_mutex_t lock;
	/* Acquired by the first buffer open, released at shutdown */
	struct dq_engine *engine;
	/* Open framing=vrt buffers; shutdown closes what the app left open */
	struct iio_buffer_pdata *vrt_bufs;
};

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

	/* framing=vrt: one engine RX queue and its poller */
	struct dq_rxq *rxq;
	struct dq_vrt_buffer *vrt;
	struct iio_buffer_pdata *next; /* in ctx->vrt_bufs */
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

const char *dq_mem_name(enum dq_mem mem);

/* C++ side (daqiri-engine.cpp). Never throw; errors are negative errnos. */
int dq_yaml_validate(const char *path, char *err, size_t errlen);
const char *dq_engine_version(void);
const char *dq_yaml_engine_kind(const char *path);

/*
 * Engine broker. One DAQIRI engine per process, initialised from the first
 * YAML acquired and never shut down before exit; acquiring another YAML
 * (compared by realpath) is -EBUSY. The first init pins this module in
 * memory so that unloading it cannot pull the code from under DAQIRI's
 * threads. Release only drops the reference.
 */
int dq_engine_acquire(const char *yaml, struct dq_engine **out, char *err, size_t errlen);
void dq_engine_release(struct dq_engine *e);
/* <iface> is an interface name or address of the YAML; empty: the first */
int dq_engine_get_port(struct dq_engine *e, const char *iface, int *port_id);

/* One owner per (port, queue): a second open is -EBUSY. UDP socket engine
 * only in this phase (-ENOTSUP otherwise). */
int dq_rxq_open(struct dq_engine *e, int port_id, int queue_id, struct dq_rxq **out);
void dq_rxq_close(struct dq_rxq *q);

struct dq_pkt {
	const uint8_t *data;
	uint32_t len;
};

/* One burst popped from a queue. l2_offset: bytes in front of the VRT
 * header (0 on the socket engine). */
struct dq_burst {
	void *handle;
	unsigned nb_pkts;
	unsigned l2_offset;
};

/* 0 with a burst to free, -EAGAIN when the queue is empty, or -errno */
int dq_rxq_poll(struct dq_rxq *q, struct dq_burst *b);
/* Fills out[] with the first min(nb_pkts, max) packets; returns the count */
unsigned dq_burst_pkts(const struct dq_burst *b, struct dq_pkt *out, unsigned max);
void dq_burst_free(struct dq_burst *b);

/* host: page aligned, THP advised; pinned: cudaHostAlloc (-ENOTSUP without
 * a usable CUDA driver); device: -ENOTSUP in this phase */
int dq_mem_alloc(enum dq_mem k, int gpu, size_t size, void **ptr);
void dq_mem_free(enum dq_mem k, void *ptr, size_t size);

/* framing=vrt streaming (daqiri-vrt.c): one RX poller thread per buffer */
extern const char *const dq_vrt_attr_names[];
extern const unsigned int dq_vrt_nb_attrs;

struct dq_vrt_buffer *dq_vrt_open(struct iio_buffer_pdata *buf, struct dq_rxq *q,
		const struct dq_opts *o, enum dq_swap swap, size_t sample_size);
void dq_vrt_close(struct dq_vrt_buffer *v);
int dq_vrt_enable(struct dq_vrt_buffer *v, bool enable);
void dq_vrt_cancel(struct dq_vrt_buffer *v);
struct dq_vrt_block *dq_vrt_block_new(struct dq_vrt_buffer *v, size_t size, void **data);
void dq_vrt_block_free(struct dq_vrt_buffer *v, struct dq_vrt_block *b);
int dq_vrt_enqueue(struct dq_vrt_buffer *v, struct dq_vrt_block *b, size_t bytes_used);
int dq_vrt_dequeue(struct dq_vrt_buffer *v, struct dq_vrt_block *b, bool nonblock, int timeout_ms);
ssize_t dq_vrt_read_attr(struct dq_vrt_buffer *v, const char *name, char *dst, size_t len);
int dq_vrt_write_attr(struct dq_vrt_buffer *v, const char *name, const char *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __DAQIRI_PRIVATE_H__ */
