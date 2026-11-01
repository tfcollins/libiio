/* SPDX-License-Identifier: MIT */
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * Where the engine shim (daqiri-engine.cpp, built into this test) says the
 * UDP sample mode datagrams of a queue go to and come from, the endpoint the
 * backend puts in its setup command: dq_yaml_udp_endpoint(), the code path of
 * dq_engine_udp_endpoint() on a YAML that is only parsed, never initialised.
 *
 * - The socket engine: local_addr and remote_addr of the loopback YAML and of
 *   the ZCU102 one, which is also checked to parse.
 * - A raw (ibverbs) engine: the UDP destination port and the IPv4 addresses of
 *   the flow that steers to each queue. DAQIRI's parser keeps a flow's
 *   addresses in network order and its ports in host order; the shim must
 *   hand both over in host order, byte-swapped neither twice nor not at all.
 *   A DAQIRI built without a raw engine cannot parse that YAML: the raw part
 *   is skipped then, and says so. The CI image builds dpdk and ibverbs.
 *
 * Usage: test_engine_udp_endpoint <resources dir>
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../daqiri-private.h"
#include "test_framework.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

struct endpoint {
	uint32_t local_ip, remote_ip;
	uint16_t local_port, remote_port;
};

static const char *dir;

static int endpoint_of(const char *file, const char *iface, int queue, struct endpoint *ep)
{
	char path[4096];

	snprintf(path, sizeof(path), "%s/%s", dir, file);

	return dq_yaml_udp_endpoint(path, iface, queue, &ep->local_ip, &ep->local_port,
			&ep->remote_ip, &ep->remote_port);
}

static void print_endpoint(const char *what, const struct endpoint *ep)
{
	printf("  %s: local %u.%u.%u.%u:%u, remote %u.%u.%u.%u:%u\n", what, ep->local_ip >> 24,
			(ep->local_ip >> 16) & 0xff, (ep->local_ip >> 8) & 0xff,
			ep->local_ip & 0xff, ep->local_port, ep->remote_ip >> 24,
			(ep->remote_ip >> 16) & 0xff, (ep->remote_ip >> 8) & 0xff,
			ep->remote_ip & 0xff, ep->remote_port);
}

/* <ret> and every field once, through a variable: TEST_ASSERT_EQ() evaluates
 * its first argument twice */
static void check(const char *what, int ret, const struct endpoint *ep, const struct endpoint *want)
{
	char msg[160];
	long v;

	snprintf(msg, sizeof(msg), "%s: found", what);
	TEST_ASSERT_EQ(ret, 0, msg);
	if (ret)
		return;

	print_endpoint(what, ep);

	v = (long)ep->local_ip;
	snprintf(msg, sizeof(msg), "%s: local address", what);
	TEST_ASSERT_EQ(v, (long)want->local_ip, msg);
	v = ep->local_port;
	snprintf(msg, sizeof(msg), "%s: local port", what);
	TEST_ASSERT_EQ(v, (long)want->local_port, msg);
	v = (long)ep->remote_ip;
	snprintf(msg, sizeof(msg), "%s: remote address", what);
	TEST_ASSERT_EQ(v, (long)want->remote_ip, msg);
	v = ep->remote_port;
	snprintf(msg, sizeof(msg), "%s: remote port", what);
	TEST_ASSERT_EQ(v, (long)want->remote_port, msg);
}

TEST_FUNCTION(socket_engine)
{
	const struct endpoint loopback = { IP(127, 0, 0, 1), IP(127, 0, 0, 1), 25101, 25102 };
	const struct endpoint zcu102 = { IP(192, 0, 2, 2), IP(192, 0, 2, 1), 4991, 4991 };
	struct endpoint ep;
	int ret;

	ret = endpoint_of("socket-udp-loopback.yaml", "", 0, &ep);
	check("socket-udp-loopback.yaml queue 0", ret, &ep, &loopback);

	/* The interface by name and by address, as iface= takes it */
	ret = endpoint_of("zcu102-socket.yaml", "data", 0, &ep);
	check("zcu102-socket.yaml, interface data, queue 0", ret, &ep, &zcu102);
	ret = endpoint_of("zcu102-socket.yaml", "192.0.2.2", 0, &ep);
	check("zcu102-socket.yaml, interface 192.0.2.2, queue 0", ret, &ep, &zcu102);

	ret = endpoint_of("zcu102-socket.yaml", "", 1, &ep);
	TEST_ASSERT_EQ(ret, -EINVAL, "zcu102-socket.yaml has no queue 1");
	ret = endpoint_of("zcu102-socket.yaml", "eth9", 0, &ep);
	TEST_ASSERT_EQ(ret, -EINVAL, "zcu102-socket.yaml has no interface eth9");
}

TEST_FUNCTION(raw_flows)
{
	/* udp_dst 4991 is 0x137f, 32531 when swapped; 192.0.2.2 would be
	 * 2.2.0.192 */
	const struct endpoint q0 = { IP(192, 0, 2, 2), IP(192, 0, 2, 1), 4991, 4990 };
	const struct endpoint q1 = { IP(198, 51, 100, 7), 0, 6001, 0 };
	char path[4096], err[256] = "";
	struct endpoint ep;
	int ret;

	snprintf(path, sizeof(path), "%s/raw-flows-ibverbs.yaml", dir);
	if (dq_yaml_validate(path, err, sizeof(err))) {
		printf("  SKIP: this DAQIRI build cannot parse raw-flows-ibverbs.yaml (%s): it has no "
		       "ibverbs engine. The flow byte order is not checked here.\n",
				err);
		return;
	}

	ret = endpoint_of("raw-flows-ibverbs.yaml", "", 0, &ep);
	check("raw-flows-ibverbs.yaml queue 0", ret, &ep, &q0);
	ret = endpoint_of("raw-flows-ibverbs.yaml", "0000:ff:1f.7", 1, &ep);
	check("raw-flows-ibverbs.yaml queue 1", ret, &ep, &q1);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		printf("Usage: %s <resources dir>\n", argv[0]);
		return 2;
	}
	dir = argv[1];

	RUN_TEST(socket_engine);
	RUN_TEST(raw_flows);

	printf("test_engine_udp_endpoint: %d/%d passed, %d failed\n", test_passed, test_count,
			test_failed);

	return test_failed ? 1 : 0;
}
