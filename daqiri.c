// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * DAQIRI backend module: daqiri:<host>[:port][,key=value]*
 * The management plane is a plain TCP iiod connection, like ip:.
 */

#include <errno.h>
#include <iio/iio-backend.h>
#include <iio/iio-debug.h>
#include <iio/iiod-client.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "daqiri-private.h"
#include "iio-config.h"
#include "iio-private.h"

#define DQ_DEFAULT_PORT "30431"
#define DQ_TIMEOUT_MS 5000

extern const struct iio_backend iio_daqiri_backend;

static const char *const dq_framing_names[] = {
	[DQ_FRAMING_VRT] = "vrt",
	[DQ_FRAMING_IIOD] = "iiod",
};

static const char *const dq_mem_names[] = {
	[DQ_MEM_HOST] = "host",
	[DQ_MEM_PINNED] = "pinned",
	[DQ_MEM_DEVICE] = "device",
};

static int dq_parse_name(const char *val, const char *const *names, unsigned int nb)
{
	unsigned int i;

	for (i = 0; i < nb; i++)
		if (!strcmp(val, names[i]))
			return (int)i;

	return -EINVAL;
}

static int dq_parse_ulong(const char *val, unsigned long max, unsigned long *out)
{
	unsigned long tmp;
	char *end;

	if (!val[0] || val[0] == '-')
		return -EINVAL;

	errno = 0;
	tmp = strtoul(val, &end, 0);
	if (*end || errno == ERANGE || tmp > max)
		return -EINVAL;

	*out = tmp;

	return 0;
}

static int dq_parse_int(const char *val, int *out)
{
	unsigned long tmp;
	int ret = dq_parse_ulong(val, INT_MAX, &tmp);

	if (!ret)
		*out = (int)tmp;
	return ret;
}

static int dq_parse_bool(const char *val, bool *out)
{
	unsigned long tmp;
	int ret = dq_parse_ulong(val, 1, &tmp);

	if (!ret)
		*out = tmp;
	return ret;
}

static int dq_parse_u32(const char *val, uint32_t *out)
{
	unsigned long tmp;
	int ret = dq_parse_ulong(val, UINT32_MAX, &tmp);

	if (!ret)
		*out = (uint32_t)tmp;
	return ret;
}

static int dq_copy_str(char *dst, size_t size, const char *val)
{
	size_t len = strlen(val);

	if (len >= size)
		return -EINVAL;

	memcpy(dst, val, len + 1);

	return 0;
}

static int dq_opts_set(struct dq_opts *o, const char *key, const char *val)
{
	unsigned long num;
	int ret;

	if (!strcmp(key, "yaml"))
		return dq_copy_str(o->yaml, sizeof(o->yaml), val);
	if (!strcmp(key, "iface"))
		return dq_copy_str(o->iface, sizeof(o->iface), val);

	if (!strcmp(key, "framing")) {
		ret = dq_parse_name(val, dq_framing_names, IIO_ARRAY_SIZE(dq_framing_names));
		if (ret < 0)
			return ret;
		o->framing = (enum dq_framing)ret;
		return 0;
	}
	if (!strcmp(key, "mem")) {
		ret = dq_parse_name(val, dq_mem_names, IIO_ARRAY_SIZE(dq_mem_names));
		if (ret < 0)
			return ret;
		o->mem = (enum dq_mem)ret;
		return 0;
	}

	if (!strcmp(key, "rxq"))
		return dq_parse_int(val, &o->rxq);
	if (!strcmp(key, "gpu"))
		return dq_parse_int(val, &o->gpu);
	if (!strcmp(key, "lossy"))
		return dq_parse_bool(val, &o->lossy);
	if (!strcmp(key, "swap"))
		return dq_parse_bool(val, &o->swap);
	if (!strcmp(key, "gap_ts"))
		return dq_parse_bool(val, &o->gap_ts);
	if (!strcmp(key, "sid"))
		return dq_parse_u32(val, &o->sid);
	if (!strcmp(key, "sid_mask"))
		return dq_parse_u32(val, &o->sid_mask);

	if (!strcmp(key, "data_port")) {
		ret = dq_parse_ulong(val, UINT16_MAX, &num);
		if (!ret)
			o->data_port = (uint16_t)num;
		return ret;
	}
	if (!strcmp(key, "poll")) {
		ret = dq_parse_ulong(val, UINT_MAX, &num);
		if (!ret)
			o->poll_us = (unsigned)num;
		return ret;
	}

	return -EINVAL;
}

int dq_opts_parse(struct dq_opts *o, const char *list, const struct iio_context_params *p)
{
	const char *env = getenv("IIO_DAQIRI_YAML");
	char *dup, *tok, *save, *val;
	int ret = 0;

	memset(o, 0, sizeof(*o));
	o->framing = DQ_FRAMING_VRT;
	o->mem = DQ_MEM_HOST;
	o->swap = true;
	o->gap_ts = true;
	o->poll_us = 50;

	if (env && dq_copy_str(o->yaml, sizeof(o->yaml), env)) {
		prm_err(p, "daqiri: IIO_DAQIRI_YAML is longer than %zu bytes\n",
				sizeof(o->yaml) - 1);
		return -EINVAL;
	}

	if (!list || !list[0])
		return 0;

	dup = iio_strdup(list);
	if (!dup)
		return -ENOMEM;

	for (tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		val = strchr(tok, '=');
		if (!val || val == tok) {
			prm_err(p, "daqiri: option \'%s\' is not key=value\n", tok);
			ret = -EINVAL;
			break;
		}

		*val++ = '\0';

		ret = dq_opts_set(o, tok, val);
		if (ret) {
			prm_err(p, "daqiri: invalid option %s=\'%s\'\n", tok, val);
			break;
		}
	}

	free(dup);

	return ret;
}

/* Host/port split copied from network.c, without the DNS-SD fallback. */
static int dq_parse_host(char *host_buf, char **host, char *port, size_t port_len)
{
	char *first_colon, *last_colon, *end, *p = NULL;
	unsigned long tmp;

	*host = host_buf;

	first_colon = strchr(host_buf, ':');
	last_colon = strrchr(host_buf, ':');

	if (!first_colon) {
		/* IPv4 or name, default port */
	} else if (first_colon == last_colon) {
		/* Single colon, IPv4 non default port */
		*first_colon = '\0';
		p = first_colon + 1;
	} else if (*(last_colon - 1) == ']') {
		/* [IPv6]:port */
		(*host)++;
		*(last_colon - 1) = '\0';
		p = last_colon + 1;
	} else {
		/* IPv6 address with default port number */
	}

	if (!(*host)[0])
		return -EINVAL;

	if (!p)
		return dq_copy_str(port, port_len, DQ_DEFAULT_PORT);

	errno = 0;
	tmp = strtoul(p, &end, 10);
	if (p == end || *end || tmp > 0xFFFF || errno == ERANGE)
		return -EINVAL;

	iio_snprintf(port, port_len, "%lu", tmp);

	return 0;
}

static void dq_peer_addr(int fd, char *dst, size_t len, const char *fallback)
{
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);

	if (getpeername(fd, (struct sockaddr *)&addr, &addrlen) ||
			getnameinfo((struct sockaddr *)&addr, addrlen, dst, (socklen_t)len, NULL, 0,
					NI_NUMERICHOST))
		iio_strlcpy(dst, fallback, len);
}

static struct iio_context *daqiri_create_context(
		const struct iio_context_params *params, const char *uri)
{
	const char *ctx_attrs[] = {
		"uri",
		"ip,ip-addr",
		"daqiri,yaml",
		"daqiri,framing",
		"daqiri,mem",
		"daqiri,engine",
		"daqiri,version",
	};
	const char *ctx_values[IIO_ARRAY_SIZE(ctx_attrs)];
	char description[sizeof("DAQIRI to ") + DQ_HOST_MAX];
	char ip_addr[INET6_ADDRSTRLEN + IF_NAMESIZE + 1];
	char host_buf[DQ_HOST_MAX], *host, *full_uri;
	const char *opts_str, *engine = "none";
	struct iio_context_pdata *pdata;
	struct iiod_client *client;
	struct iio_context *ctx;
	char err[256];
	size_t host_len;
	int ret;

	pdata = zalloc(sizeof(*pdata));
	if (!pdata)
		return iio_ptr(-ENOMEM);

	pdata->mgmt.fd = -1;
	pdata->mgmt.cancel_fd = -1;
	pdata->mgmt.params = params;

	/* Options first, so that a bad URI fails without touching the network */
	opts_str = strchr(uri, ',');
	ret = dq_opts_parse(&pdata->opts, opts_str ? opts_str + 1 : NULL, params);
	if (ret)
		goto err_free_pdata;

	host_len = opts_str ? (size_t)(opts_str - uri) : strlen(uri);
	ret = -EINVAL;
	if (host_len < sizeof(host_buf)) {
		memcpy(host_buf, uri, host_len);
		host_buf[host_len] = '\0';
		ret = dq_parse_host(host_buf, &host, pdata->port, sizeof(pdata->port));
	}
	if (ret) {
		prm_err(params, "daqiri: invalid host in \'%s\'\n", uri);
		goto err_free_pdata;
	}

	iio_strlcpy(pdata->host, host, sizeof(pdata->host));

	if (pdata->opts.yaml[0]) {
		ret = dq_yaml_validate(pdata->opts.yaml, err, sizeof(err));
		if (ret) {
			prm_err(params, "daqiri: invalid yaml %s: %s\n", pdata->opts.yaml, err);
			ret = -EINVAL;
			goto err_free_pdata;
		}

		engine = dq_yaml_engine_kind(pdata->opts.yaml);
	}

	ret = dq_mgmt_connect(&pdata->mgmt, pdata->host, pdata->port, params->timeout_ms);
	if (ret)
		goto err_free_pdata;

	client = iiod_client_new(params, (struct iiod_client_pdata *)&pdata->mgmt, &dq_mgmt_ops);
	ret = iio_err(client);
	if (ret)
		goto err_mgmt_close;

	pdata->client = client;

	full_uri = malloc(sizeof("daqiri:") + strlen(uri));
	if (!full_uri) {
		ret = -ENOMEM;
		goto err_destroy_client;
	}
	iio_snprintf(full_uri, sizeof("daqiri:") + strlen(uri), "daqiri:%s", uri);

	dq_peer_addr(pdata->mgmt.fd, ip_addr, sizeof(ip_addr), pdata->host);
	iio_snprintf(description, sizeof(description), "DAQIRI to %s", pdata->host);

	ctx_values[0] = full_uri;
	ctx_values[1] = ip_addr;
	ctx_values[2] = pdata->opts.yaml;
	ctx_values[3] = dq_framing_names[pdata->opts.framing];
	ctx_values[4] = dq_mem_names[pdata->opts.mem];
	ctx_values[5] = engine;
	ctx_values[6] = dq_engine_version();

	prm_dbg(params, "Creating context...\n");
	ctx = iiod_client_create_context(client, &iio_daqiri_backend, description, ctx_attrs,
			ctx_values, IIO_ARRAY_SIZE(ctx_values));
	free(full_uri);
	ret = iio_err(ctx);
	if (ret)
		goto err_destroy_client;

	iio_context_set_pdata(ctx, pdata);

	/* pdata->mgmt.params points to the 'params' function argument,
	 * switch it to the context's copy now */
	pdata->mgmt.params = iio_context_get_params(ctx);

	return ctx;

err_destroy_client:
	dq_mgmt_ops.cancel((struct iiod_client_pdata *)&pdata->mgmt);
	iiod_client_destroy(client);
err_mgmt_close:
	dq_mgmt_close(&pdata->mgmt);
err_free_pdata:
	free(pdata);
	return iio_ptr(ret);
}

static struct iio_context_pdata *dq_dev_pdata(const struct iio_device *dev)
{
	return iio_context_get_pdata(iio_device_get_context(dev));
}

static ssize_t daqiri_read_attr(const struct iio_attr *attr, char *dst, size_t len)
{
	const struct iio_device *dev = iio_attr_get_device(attr);

	return iiod_client_attr_read(dq_dev_pdata(dev)->client, attr, dst, len);
}

static ssize_t daqiri_write_attr(const struct iio_attr *attr, const char *src, size_t len)
{
	const struct iio_device *dev = iio_attr_get_device(attr);

	return iiod_client_attr_write(dq_dev_pdata(dev)->client, attr, src, len);
}

static const struct iio_device *daqiri_get_trigger(const struct iio_device *dev)
{
	return iiod_client_get_trigger(dq_dev_pdata(dev)->client, dev);
}

static int daqiri_set_trigger(const struct iio_device *dev, const struct iio_device *trigger)
{
	return iiod_client_set_trigger(dq_dev_pdata(dev)->client, dev, trigger);
}

static void daqiri_shutdown(struct iio_context *ctx)
{
	struct iio_context_pdata *pdata = iio_context_get_pdata(ctx);

	/* The core frees pdata itself after this returns */
	dq_mgmt_ops.cancel((struct iiod_client_pdata *)&pdata->mgmt);
	iiod_client_destroy(pdata->client);
	dq_mgmt_close(&pdata->mgmt);
}

static int daqiri_set_timeout(struct iio_context *ctx, int timeout)
{
	struct iio_context_pdata *pdata = iio_context_get_pdata(ctx);
	int ret;

	ret = iiod_client_set_timeout(pdata->client, timeout);
	if (ret < 0) {
		ctx_perror(ctx, ret, "Unable to set R/W timeout");
		return ret;
	}

	return 0;
}

static struct iio_event_stream_pdata *daqiri_open_events_fd(const struct iio_device *dev)
{
	return iiod_client_open_event_stream(dq_dev_pdata(dev)->client, dev);
}

static int daqiri_reg_read(const struct iio_device *dev, uint32_t address, uint32_t *value)
{
	struct iiod_client *client = dq_dev_pdata(dev)->client;

	if (!iiod_client_uses_binary_interface(client)) {
		/* Fallback to attribute-based method for ASCII protocol */
		return -ENOSYS;
	}

	return iiod_client_reg_read(client, dev, address, value);
}

static int daqiri_reg_write(const struct iio_device *dev, uint32_t address, uint32_t value)
{
	struct iiod_client *client = dq_dev_pdata(dev)->client;

	if (!iiod_client_uses_binary_interface(client)) {
		/* Fallback to attribute-based method for ASCII protocol */
		return -ENOSYS;
	}

	return iiod_client_reg_write(client, dev, address, value);
}

static int daqiri_refresh_format(const struct iio_channel *chn)
{
	const struct iio_device *dev = iio_channel_get_device(chn);
	struct iio_context_pdata *pdata = dq_dev_pdata(dev);
	struct iio_data_format new_format;
	struct iio_channel *chn_rw = (struct iio_channel *)chn;
	char format_str[256];
	ssize_t ret;

	if (!iiod_client_uses_binary_interface(pdata->client))
		return -ENOTSUP;

	ret = iiod_client_refresh_format(pdata->client, dev, chn, format_str, sizeof(format_str));
	if (ret < 0)
		return (int)ret;

	format_str[ret] = '\0';

	ret = iio_parse_format_string(format_str, &new_format);
	if (ret < 0)
		return (int)ret;

	new_format.with_scale = chn->format.with_scale;
	new_format.scale = chn->format.scale;
	new_format.offset = chn->format.offset;

	chn_rw->format = new_format;

	return 0;
}

static int daqiri_ping(struct iio_context *ctx)
{
	struct iio_context_pdata *pdata = iio_context_get_pdata(ctx);

	return iiod_client_nop(pdata->client);
}

static const struct iio_backend_ops daqiri_ops = {
	.create = daqiri_create_context,
	.read_attr = daqiri_read_attr,
	.write_attr = daqiri_write_attr,
	.get_trigger = daqiri_get_trigger,
	.set_trigger = daqiri_set_trigger,
	.shutdown = daqiri_shutdown,
	.set_timeout = daqiri_set_timeout,

	.open_ev = daqiri_open_events_fd,
	.close_ev = iiod_client_close_event_stream,
	.read_ev = iiod_client_read_event,

	.reg_read = daqiri_reg_read,
	.reg_write = daqiri_reg_write,

	.refresh_format = daqiri_refresh_format,

	.ping = daqiri_ping,
};

__api_export_if(WITH_DAQIRI_BACKEND) const struct iio_backend iio_daqiri_backend = {
	.api_version = IIO_BACKEND_API_V1,
	.name = "daqiri",
	.uri_prefix = "daqiri:",
	.ops = &daqiri_ops,
	.default_timeout_ms = DQ_TIMEOUT_MS,
};
