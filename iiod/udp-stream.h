/* SPDX-License-Identifier: MIT */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Server side of the UDP sample stream. A provider registered here answers
 * IIOD_OP_UDP_STREAM_SETUP and sends the samples of an enabled buffer as UDP
 * datagrams. Without one, the command is answered with -ENOTSUP, which is what
 * iiod, tinyiiod and Zephyr do by default.
 */

#ifndef __IIOD_SERVER_UDP_STREAM_H__
#define __IIOD_SERVER_UDP_STREAM_H__

#include "../iiod-udp-stream.h"

/* See ops.h. A provider keeps its per-buffer state in entry->udp_priv. */
struct buffer_entry;

/*
 * Each is called from the thread serving the connection that sent the
 * command; for release(), that is the one that sent CLOSE_BUFFER, usually the
 * context's connection rather than the buffer's. On connection teardown,
 * release() runs with the buffer list locked: it must not call back into the
 * responder.
 */
struct iiod_udp_stream_ops {
	/* IIOD_OP_UDP_STREAM_SETUP on an open buffer. Check <req>, set the stream
	 * up and fill <resp> (zeroed on entry). Returns 0 or a negative error
	 * code, which is what the client gets. It may come again before the
	 * buffer is enabled; if a later one fails, the earlier success still
	 * counts and start() follows the enable. */
	int (*setup)(void *priv, struct buffer_entry *entry, const struct iiod_udp_stream_req *req,
			struct iiod_udp_stream_resp *resp);

	/* The buffer was just enabled, and a setup succeeded since it was last
	 * disabled: start sending. A negative return fails the enable; the buffer
	 * stays enabled and stop() follows when it is disabled. */
	int (*start)(void *priv, struct buffer_entry *entry);

	/* The buffer is about to be disabled, and a setup succeeded since it was
	 * last disabled: stop sending. Also called when start() failed or never
	 * ran. The setup is forgotten afterwards. */
	void (*stop)(void *priv, struct buffer_entry *entry);

	/* The buffer is closed, or its connection is gone, and entry->udp_priv is
	 * set: stop sending if still running, and free entry->udp_priv. */
	void (*release)(void *priv, struct buffer_entry *entry);
};

/* Register the provider, with <priv> passed back to every call; NULL removes
 * it. Call it before the server takes connections. */
void iiod_udp_stream_set_ops(const struct iiod_udp_stream_ops *ops, void *priv);

#endif /* __IIOD_SERVER_UDP_STREAM_H__ */
