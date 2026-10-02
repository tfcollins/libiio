// SPDX-License-Identifier: LGPL-2.1-or-later
/* Experimental DQI1 backend. Management is an independently owned iiod
 * connection; no network backend objects or private layouts are borrowed. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <iio/iiod-client.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "daqiri-private.h"

struct iiod_client_pdata {
	int fd;
};
struct iio_context_pdata {
	struct iiod_client_pdata io;
	struct iiod_client *client;
	struct iio_context_params params;
	struct dq_profile profile;
	pthread_mutex_t lock;
	bool busy;
};
struct iio_buffer_pdata {
	struct iio_context_pdata *owner;
	struct dq_runtime *runtime;
	struct dq_stream *stream;
};
struct iio_block_pdata {
	struct dq_block *block;
};
extern const struct iio_backend iio_daqiri_backend;

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_socket(int fd, short events, int64_t deadline)
{
	struct pollfd pfd = { .fd = fd, .events = events };
	int ret;
	for (;;) {
		int64_t left = deadline < 0 ? -1 : deadline - now_ms();
		if (deadline >= 0 && left <= 0)
			return -ETIMEDOUT;
		ret = poll(&pfd, 1, left > INT32_MAX ? INT32_MAX : (int)left);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			return -errno;
		if (!ret)
			return -ETIMEDOUT;
		if (pfd.revents & events)
			return 0;
		return -EPIPE;
	}
}

static ssize_t management_io(
		struct iiod_client_pdata *io, void *data, size_t len, int timeout, bool write_data)
{
	int64_t deadline = timeout < 0 ? -1 : now_ms() + timeout;
	ssize_t ret;
	int err;
	if (!len)
		return 0;
	for (;;) {
		err = wait_socket(io->fd, write_data ? POLLOUT : POLLIN, deadline);
		if (err)
			return err;
		ret = write_data ? send(io->fd, data, len, MSG_NOSIGNAL)
				 : recv(io->fd, data, len, 0);
		if (ret < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		return ret < 0 ? -errno : ret ? ret : -EPIPE;
	}
}
static ssize_t management_read(struct iiod_client_pdata *io, char *dst, size_t len, int timeout)
{
	return management_io(io, dst, len, timeout, false);
}
static ssize_t management_write(
		struct iiod_client_pdata *io, const char *src, size_t len, int timeout)
{
	return management_io(io, (void *)src, len, timeout, true);
}
static void management_cancel(struct iiod_client_pdata *io)
{
	shutdown(io->fd, SHUT_RDWR);
}
static const struct iiod_client_ops management_ops = {
	.read = management_read,
	.write = management_write,
	.cancel = management_cancel,
};

static struct iio_context_pdata *device_owner(const struct iio_device *dev)
{
	return iio_context_get_pdata(iio_device_get_context(dev));
}
static ssize_t daqiri_read_attr(const struct iio_attr *attr, char *dst, size_t len)
{
	const struct iio_device *dev = iio_attr_get_device(attr);
	if (!dev)
		return -ENOTSUP;
	return iiod_client_attr_read(device_owner(dev)->client, attr, dst, len);
}
static ssize_t daqiri_write_attr(const struct iio_attr *attr, const char *src, size_t len)
{
	const struct iio_device *dev = iio_attr_get_device(attr);
	struct iio_context_pdata *p;
	ssize_t ret;
	if (!dev)
		return -ENOTSUP;
	p = device_owner(dev);
	pthread_mutex_lock(&p->lock);
	/* A running layout cannot be changed behind the negotiated contract. */
	ret = p->busy ? -EBUSY : iiod_client_attr_write(p->client, attr, src, len);
	pthread_mutex_unlock(&p->lock);
	return ret;
}
static const struct iio_device *daqiri_get_trigger(const struct iio_device *dev)
{
	return iiod_client_get_trigger(device_owner(dev)->client, dev);
}
static int daqiri_set_trigger(const struct iio_device *dev, const struct iio_device *trigger)
{
	struct iio_context_pdata *p = device_owner(dev);
	int ret;
	pthread_mutex_lock(&p->lock);
	ret = p->busy ? -EBUSY : iiod_client_set_trigger(p->client, dev, trigger);
	pthread_mutex_unlock(&p->lock);
	return ret;
}
static int daqiri_ping(struct iio_context *ctx)
{
	return iiod_client_nop(iio_context_get_pdata(ctx)->client);
}
static int daqiri_set_timeout(struct iio_context *ctx, int timeout)
{
	struct iio_context_pdata *p = iio_context_get_pdata(ctx);
	int ret;
	if (timeout <= 0 || timeout > 60000)
		return -EINVAL;
	pthread_mutex_lock(&p->lock);
	ret = p->busy ? -EBUSY : iiod_client_set_timeout(p->client, timeout);
	if (!ret) {
		p->profile.timeout_ms = timeout;
		p->params.timeout_ms = timeout;
	}
	pthread_mutex_unlock(&p->lock);
	return ret;
}
static void daqiri_shutdown(struct iio_context *ctx)
{
	struct iio_context_pdata *p = iio_context_get_pdata(ctx);
	/* XML parsing can destroy a partially constructed context. */
	if (!p)
		return;
	management_cancel(&p->io);
	iiod_client_destroy(p->client);
	close(p->io.fd);
	pthread_mutex_destroy(&p->lock);
	/* libiio owns and frees the context pdata. Buffers must be closed first. */
}

static struct iio_context *daqiri_create(
		const struct iio_context_params *params, const char *profile)
{
	struct iio_context_pdata *p = calloc(1, sizeof(*p));
	struct sockaddr_in address = { .sin_family = AF_INET };
	struct iio_context *ctx;
	const struct iio_attr *cap;
	const char *value;
	char uri[80];
	const char *names[] = { "uri" }, *values[] = { uri };
	int ret, error;
	socklen_t error_len = sizeof(error);
	if (!p)
		return iio_ptr(-ENOMEM);
	ret = dq_profile_load(profile, &p->profile);
	if (ret)
		goto free_p;
	p->params = *params;
	if (!p->params.timeout_ms)
		p->params.timeout_ms = p->profile.timeout_ms;
	if (p->params.timeout_ms > 60000) {
		ret = -EINVAL;
		goto free_p;
	}
	p->profile.timeout_ms = p->params.timeout_ms;
	ret = pthread_mutex_init(&p->lock, NULL);
	if (ret) {
		ret = -ret;
		goto free_p;
	}
	p->io.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (p->io.fd < 0) {
		ret = -errno;
		goto destroy_lock;
	}
	address.sin_port = htons(p->profile.management_port);
	if (inet_pton(AF_INET, p->profile.management, &address.sin_addr) != 1) {
		ret = -EINVAL;
		goto close_socket;
	}
	ret = connect(p->io.fd, (struct sockaddr *)&address, sizeof(address));
	if (ret < 0) {
		if (errno != EINPROGRESS) {
			ret = -errno;
			goto close_socket;
		}
		ret = wait_socket(p->io.fd, POLLOUT, now_ms() + p->params.timeout_ms);
		if (ret)
			goto close_socket;
		if (getsockopt(p->io.fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
			ret = -errno;
			goto close_socket;
		}
		if (error) {
			ret = -error;
			goto close_socket;
		}
	}
	p->client = iiod_client_new(&p->params, &p->io, &management_ops);
	ret = iio_err(p->client);
	if (ret)
		goto close_socket;
	snprintf(uri, sizeof(uri), "daqiri:%s", profile);
	ctx = iiod_client_create_context(p->client, &iio_daqiri_backend,
			"DAQIRI DQI1 (experimental, CPU-only)", names, values, 1);
	ret = iio_err(ctx);
	if (ret)
		goto destroy_client;
	iio_context_set_pdata(ctx, p);
	/* Never initialize the packet engine against a legacy/VRT-only peer. */
	cap = iio_context_find_attr(ctx, "daqiri.protocol");
	value = cap ? iio_attr_get_static_value(cap) : NULL;
	if (!value || strcmp(value, DQ_CAPABILITY)) {
		iio_context_destroy(ctx);
		return iio_ptr(-EPROTONOSUPPORT);
	}
	return ctx;
destroy_client:
	management_cancel(&p->io);
	iiod_client_destroy(p->client);
close_socket:
	close(p->io.fd);
destroy_lock:
	pthread_mutex_destroy(&p->lock);
free_p:
	free(p);
	return iio_ptr(ret);
}

/* HELLO description: "iio-layout-v1\n<device>\n" followed by one ASCII
 * record per enabled channel: ordinal:index:be:signed:bits:length:shift:repeat.
 * Order is libiio channel order. Peer must echo the entire contract exactly.
 * Only byte-addressable integer formats are supported in this first version. */
static struct iio_buffer_pdata *daqiri_open_buffer(
		const struct iio_device *dev, unsigned int idx, struct iio_channels_mask *mask)
{
	struct iio_context_pdata *p = device_owner(dev);
	struct iio_buffer_pdata *b;
	struct dq_io io;
	char description[1024];
	const char *id = iio_device_get_id(dev);
	ssize_t stride;
	int ret, tx = -1, n;
	size_t used;
	unsigned int i;
	if (idx || strcmp(id, p->profile.device))
		return iio_ptr(-ENOTSUP);
	if (strchr(id, '\n') || strchr(id, '\r'))
		return iio_ptr(-EINVAL);
	stride = iio_device_get_sample_size(dev, mask);
	if (stride <= 0 || stride > UINT32_MAX)
		return iio_ptr(-EINVAL);
	n = snprintf(description, sizeof(description), "iio-layout-v1\n%s\n", id);
	if (n < 0 || (size_t)n >= sizeof(description))
		return iio_ptr(-E2BIG);
	used = (size_t)n;
	for (i = 0; i < iio_device_get_channels_count(dev); i++) {
		const struct iio_channel *ch = iio_device_get_channel(dev, i);
		const struct iio_data_format *f;
		int direction;
		if (!iio_channel_is_enabled(ch, mask))
			continue;
		if (!iio_channel_is_scan_element(ch))
			return iio_ptr(-EINVAL);
		direction = iio_channel_is_output(ch);
		if (tx >= 0 && tx != direction)
			return iio_ptr(-ENOTSUP);
		tx = direction;
		f = iio_channel_get_data_format(ch);
		if (!f->length || f->length % 8 || !f->bits || f->bits > f->length ||
				f->shift > f->length - f->bits || !f->repeat)
			return iio_ptr(-ENOTSUP);
		n = snprintf(description + used, sizeof(description) - used,
				"%u:%ld:%u:%u:%u:%u:%u:%u\n", i, iio_channel_get_index(ch),
				(unsigned)f->is_be, (unsigned)f->is_signed, f->bits, f->length,
				f->shift, f->repeat);
		if (n < 0 || (size_t)n >= sizeof(description) - used)
			return iio_ptr(-E2BIG);
		used += (size_t)n;
	}
	if (tx < 0)
		return iio_ptr(-EINVAL);
	b = calloc(1, sizeof(*b));
	if (!b)
		return iio_ptr(-ENOMEM);
	b->owner = p;
	pthread_mutex_lock(&p->lock);
	if (p->busy) {
		ret = -EBUSY;
		goto fail;
	}
	ret = dq_runtime_open(&p->profile, &b->runtime, &io);
	if (ret)
		goto fail;
	ret = dq_stream_open(&io, &p->profile, tx, (size_t)stride, description, used, &b->stream);
	if (ret) {
		dq_runtime_close(b->runtime);
		goto fail;
	}
	p->busy = true;
	pthread_mutex_unlock(&p->lock);
	return b;
fail:
	pthread_mutex_unlock(&p->lock);
	free(b);
	return iio_ptr(ret);
}
static void daqiri_close_buffer(struct iio_buffer_pdata *b)
{
	struct iio_context_pdata *p = b->owner;
	pthread_mutex_lock(&p->lock);
	dq_stream_close(b->stream);
	dq_runtime_close(b->runtime);
	p->busy = false;
	pthread_mutex_unlock(&p->lock);
	free(b);
}
static void daqiri_cancel_buffer(struct iio_buffer_pdata *b)
{
	dq_stream_cancel(b->stream);
}
static int daqiri_enable_buffer(
		struct iio_buffer_pdata *b, size_t samples, bool enable, bool cyclic)
{
	(void)samples;
	return cyclic ? -ENOTSUP : dq_stream_enable(b->stream, enable);
}
static struct iio_block_pdata *daqiri_create_block(
		struct iio_buffer_pdata *b, size_t size, void **data)
{
	struct iio_block_pdata *block = calloc(1, sizeof(*block));
	int ret;
	if (!block)
		return iio_ptr(-ENOMEM);
	ret = dq_block_create(b->stream, size, &block->block, data);
	if (ret) {
		free(block);
		return iio_ptr(ret);
	}
	return block;
}
static void daqiri_free_block(struct iio_block_pdata *block)
{
	dq_block_free(block->block);
	free(block);
}
static int daqiri_enqueue_block(struct iio_block_pdata *block, size_t used, bool cyclic)
{
	return cyclic ? -ENOTSUP : dq_block_enqueue(block->block, used, false);
}
static int daqiri_dequeue_block(struct iio_block_pdata *block, bool nonblock)
{
	return dq_block_dequeue(block->block, nonblock);
}
static int daqiri_get_dmabuf_fd(struct iio_block_pdata *block)
{
	(void)block;
	return -ENOTSUP;
}
static int daqiri_disable_cpu_access(struct iio_block_pdata *block, bool disable)
{
	(void)block;
	(void)disable;
	return -ENOTSUP;
}
static const struct iio_backend_ops daqiri_ops = {
	.create = daqiri_create,
	.shutdown = daqiri_shutdown,
	.read_attr = daqiri_read_attr,
	.write_attr = daqiri_write_attr,
	.get_trigger = daqiri_get_trigger,
	.set_trigger = daqiri_set_trigger,
	.set_timeout = daqiri_set_timeout,
	.ping = daqiri_ping,
	.open_buffer = daqiri_open_buffer,
	.close_buffer = daqiri_close_buffer,
	.enable_buffer = daqiri_enable_buffer,
	.cancel_buffer = daqiri_cancel_buffer,
	.create_block = daqiri_create_block,
	.free_block = daqiri_free_block,
	.enqueue_block = daqiri_enqueue_block,
	.dequeue_block = daqiri_dequeue_block,
	.get_dmabuf_fd = daqiri_get_dmabuf_fd,
	.disable_cpu_access = daqiri_disable_cpu_access,
};
const struct iio_backend iio_daqiri_backend = {
	.api_version = IIO_BACKEND_API_V1,
	.name = "daqiri",
	.uri_prefix = "daqiri:",
	.ops = &daqiri_ops,
	.default_timeout_ms = 1000,
};
