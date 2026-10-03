/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef IIO_DAQIRI_PRIVATE_H
#define IIO_DAQIRI_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DQ_MAX_PACKET 8192
#define DQ_HEADER 40
#define DQ_CAPABILITY "dqi1-cpu-strict-credit-v1"

struct dq_profile {
	char management[64];
	char yaml[512];
	char interface_name[64];
	char device[128];

	char local_ip[16];
	char peer_ip[16];
	char peer_mac[18];

	uint16_t management_port;
	uint16_t local_port;
	uint16_t peer_port;

	uint32_t payload;
	uint32_t max_block;
	uint32_t max_blocks;
	uint32_t timeout_ms;
};

/* Packet transport: zero on send success, -EAGAIN on temporary scarcity;
 * receive returns datagram length, zero when empty, negative errno on error.
 * Both lanes must have independent queues and packet pools. */
struct dq_io {
	void *opaque;
	int (*send)(void *, int control, const void *, size_t);
	int (*recv)(void *, int control, void *, size_t);
};

struct dq_runtime;
struct dq_stream;
struct dq_block;

int dq_profile_load(const char *name, struct dq_profile *out);

int dq_runtime_open(const struct dq_profile *, struct dq_runtime **out, struct dq_io *io);
void dq_runtime_close(struct dq_runtime *);

int dq_stream_open(const struct dq_io *, const struct dq_profile *, int tx, size_t stride,
        const void *description, size_t description_size, struct dq_stream **out);
void dq_stream_close(struct dq_stream *);
int dq_stream_enable(struct dq_stream *, int enable);
void dq_stream_cancel(struct dq_stream *);

int dq_block_create(struct dq_stream *, size_t size, struct dq_block **out, void **data);
void dq_block_free(struct dq_block *);
int dq_block_enqueue(struct dq_block *, size_t used, int cyclic);
int dq_block_dequeue(struct dq_block *, int nonblock);

#ifdef __cplusplus
}
#endif

#endif
