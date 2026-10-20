// SPDX-License-Identifier: MIT
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 *
 * vrt_sender: a standalone VITA 49.2 software framer for the DAQIRI backend
 * tests. It sends IF data packets over UDP, can drop, reorder and duplicate
 * them on purpose, and writes what it did as a JSON oracle. POSIX/Linux and
 * libc only; see usage() for the options and the exact rules.
 */

#define _GNU_SOURCE 1 /* sendmmsg(), getopt_long() */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BATCH 32

/* Header, Stream ID, Class ID (2 words), TSI, TSF (2 words) */
#define PROLOGUE_BYTES 28
/* Largest IPv4 UDP payload */
#define MAX_DGRAM 65507
#define MAX_SAMPLES ((MAX_DGRAM - PROLOGUE_BYTES) / 4)

/* Same values as tests/daqiri/gen_golden.py */
#define OUI 0x000456u
#define PACKET_CLASS 0x0010u /* vrt49 CI16 */
#define TSI_SECS 1700000000u

#define DEFAULT_FROM "127.0.0.1:25102"
#define DEFAULT_SAMPLES 360
#define DEFAULT_SID 0x01000000u

#define EXIT_USAGE 2

struct u64_list {
	uint64_t *v;
	size_t len, cap;
};

struct opts {
	struct sockaddr_in to, from;
	bool have_to;
	uint64_t samples;
	uint32_t sid;
	uint64_t packets; /* 0: --duration */
	double duration;
	uint64_t pps; /* 0: unpaced */
	const char *source;
	struct u64_list drop_list; /* sorted */
	uint64_t drop_every;
	double drop_prob;
	uint64_t seed;
	uint64_t reorder; /* K; 0: off */
	uint64_t dup_every;
	uint64_t burst_gap_ms;
	const char *json;
};

/* The payload source: NULL data is ramp16 */
struct source {
	uint8_t *data;
	size_t len;
};

/* The packet generator, in emission order */
struct gen {
	const struct opts *o;
	struct timespec start;
	uint64_t next;	  /* next packet index to consider */
	uint64_t pend[4]; /* indices ready to send: a pair, each maybe twice */
	unsigned int nb_pend, pos;
	struct u64_list dropped, reordered, duplicated;
};

static void usage(FILE *f)
{
	fprintf(f,
			"Usage: vrt_sender --to IP:PORT (--packets N | --duration S) [options]\n"
			"\n"
			"Sends VITA 49.2 IF data packets over UDP: Stream ID, Class ID (OUI 0x000456,\n"
			"packet class 0x0010 = CI16), TSI UTC (fixed at %u), TSF sample count =\n"
			"index of the packet's first sample (index * samples), 4-bit packet count =\n"
			"index & 0xF. Each sample is two big-endian 16-bit words, I then Q.\n"
			"\n"
			"Options:\n"
			"  --to IP:PORT         destination (required)\n"
			"  --from IP:PORT       source to bind, with SO_REUSEADDR (default %s)\n"
			"  --samples N          samples per packet, 1..%d (default %d)\n"
			"  --sid N              Stream ID (default 0x%08x)\n"
			"  --packets N          send packet indices 0 .. N-1\n"
			"  --duration S         or start new packets for S seconds\n"
			"  --pps N              datagrams per second, paced with clock_nanosleep\n"
			"                       (default: as fast as possible)\n"
			"  --source ramp|FILE   payload. ramp (default): sample n of the stream has\n"
			"                       I = n & 0x7FFF, Q = (0x8000 - n) & 0xFFFF, continuing\n"
			"                       across packets. FILE: the file's bytes, replayed\n"
			"                       cyclically and sent as they are\n"
			"  --drop-list A,B,...  drop these packet indices\n"
			"  --drop-every N       drop indices N-1, 2N-1, 3N-1, ...\n"
			"  --drop-prob P        drop each index with probability P (0..1); the\n"
			"                       decision is a function of --seed and the index\n"
			"  --seed S             seed for --drop-prob (default 1)\n"
			"  --reorder-window K   K >= 2: for every m >= 1, send packet mK just before\n"
			"                       packet mK-1 (pairs with a dropped packet are kept\n"
			"                       in order)\n"
			"  --dup-every N        send indices N-1, 2N-1, ... twice in a row\n"
			"  --burst-gap-ms M     pause M ms after every %d datagrams\n"
			"  --json PATH          write the oracle there when done\n"
			"  --help               this text\n"
			"\n"
			"A dropped packet still takes its index: its count and TSF are skipped,\n"
			"which is what makes the loss detectable.\n"
			"\n"
			"Emission order: indices ascending, dropped ones skipped, a reordered pair\n"
			"as mK then mK-1, a duplicated packet directly followed by its copy. The\n"
			"datagrams go out with sendmmsg in batches of up to %d; with --pps, datagram\n"
			"d is due d/N s after the start (or after the last burst gap).\n"
			"\n"
			"Oracle: {\"sent\": D, \"dropped\": [i, ...], \"reordered\": [[a, b], ...],\n"
			"\"duplicated\": [i, ...]}. D counts datagrams sent, copies included; the\n"
			"lists hold packet indices; [a, b] means b = a + 1 was sent before a.\n"
			"\n"
			"Exit status: 0 done, 1 runtime error, 2 usage error.\n",
			TSI_SECS, DEFAULT_FROM, MAX_SAMPLES, DEFAULT_SAMPLES, DEFAULT_SID, BATCH,
			BATCH);
}

static void __attribute__((format(printf, 1, 2), noreturn)) usage_error(const char *fmt, ...);

static void usage_error(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "vrt_sender: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\nTry 'vrt_sender --help'.\n");
	exit(EXIT_USAGE);
}

static int list_add(struct u64_list *l, uint64_t v)
{
	uint64_t *p;
	size_t cap;

	if (l->len == l->cap) {
		cap = l->cap ? 2 * l->cap : 64;
		p = realloc(l->v, cap * sizeof(*p));
		if (!p)
			return -ENOMEM;
		l->v = p;
		l->cap = cap;
	}
	l->v[l->len++] = v;

	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/* A whole decimal or 0x number in [min, max], else a usage error */
static uint64_t parse_u64(const char *opt, const char *s, uint64_t min, uint64_t max)
{
	unsigned long long v;
	char *end;

	errno = 0;
	if (!s[0] || s[0] == '-' || s[0] == '+' || s[0] == ' ')
		usage_error("%s: '%s' is not a number", opt, s);
	v = strtoull(s, &end, 0);
	if (errno || *end || end == s)
		usage_error("%s: '%s' is not a number", opt, s);
	if (v < min || v > max)
		usage_error("%s: %s is not in %" PRIu64 "..%" PRIu64, opt, s, min, max);

	return (uint64_t)v;
}

static double parse_double(const char *opt, const char *s, double min, double max)
{
	char *end;
	double v;

	errno = 0;
	v = strtod(s, &end);
	if (errno || *end || end == s || !isfinite(v))
		usage_error("%s: '%s' is not a number", opt, s);
	if (v < min || v > max)
		usage_error("%s: %s is not in %g..%g", opt, s, min, max);

	return v;
}

static void parse_addr(const char *opt, const char *s, uint64_t min_port, struct sockaddr_in *sa)
{
	const char *colon = strrchr(s, ':');
	char ip[INET_ADDRSTRLEN];

	if (!colon || (size_t)(colon - s) >= sizeof(ip))
		usage_error("%s: '%s' is not IPv4:PORT", opt, s);

	memcpy(ip, s, (size_t)(colon - s));
	ip[colon - s] = '\0';

	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	if (inet_pton(AF_INET, ip, &sa->sin_addr) != 1)
		usage_error("%s: '%s' is not an IPv4 address", opt, ip);
	sa->sin_port = htons((uint16_t)parse_u64(opt, colon + 1, min_port, 65535));
}

static void parse_list(const char *opt, const char *s, struct u64_list *l)
{
	char *dup = strdup(s), *tok, *save = NULL, *p;

	if (!dup) {
		perror("vrt_sender");
		exit(1);
	}

	/* strtok_r() skips empty fields: reject them first */
	if (!s[0] || s[0] == ',' || s[strlen(s) - 1] == ',' || strstr(s, ",,"))
		usage_error("%s: '%s' is not a list of indices", opt, s);

	for (p = dup; (tok = strtok_r(p, ",", &save)); p = NULL) {
		if (list_add(l, parse_u64(opt, tok, 0, UINT64_MAX))) {
			perror("vrt_sender");
			exit(1);
		}
	}
	free(dup);

	qsort(l->v, l->len, sizeof(*l->v), cmp_u64);
}

static void parse_opts(int argc, char **argv, struct opts *o)
{
	enum {
		OPT_TO = 256,
		OPT_FROM,
		OPT_SAMPLES,
		OPT_SID,
		OPT_PACKETS,
		OPT_DURATION,
		OPT_PPS,
		OPT_SOURCE,
		OPT_DROP_LIST,
		OPT_DROP_EVERY,
		OPT_DROP_PROB,
		OPT_SEED,
		OPT_REORDER,
		OPT_DUP_EVERY,
		OPT_BURST_GAP,
		OPT_JSON,
		OPT_HELP,
	};
	static const struct option longopts[] = {
		{ "to", required_argument, NULL, OPT_TO },
		{ "from", required_argument, NULL, OPT_FROM },
		{ "samples", required_argument, NULL, OPT_SAMPLES },
		{ "sid", required_argument, NULL, OPT_SID },
		{ "packets", required_argument, NULL, OPT_PACKETS },
		{ "duration", required_argument, NULL, OPT_DURATION },
		{ "pps", required_argument, NULL, OPT_PPS },
		{ "source", required_argument, NULL, OPT_SOURCE },
		{ "drop-list", required_argument, NULL, OPT_DROP_LIST },
		{ "drop-every", required_argument, NULL, OPT_DROP_EVERY },
		{ "drop-prob", required_argument, NULL, OPT_DROP_PROB },
		{ "seed", required_argument, NULL, OPT_SEED },
		{ "reorder-window", required_argument, NULL, OPT_REORDER },
		{ "dup-every", required_argument, NULL, OPT_DUP_EVERY },
		{ "burst-gap-ms", required_argument, NULL, OPT_BURST_GAP },
		{ "json", required_argument, NULL, OPT_JSON },
		{ "help", no_argument, NULL, OPT_HELP },
		{ NULL, 0, NULL, 0 },
	};
	const char *name;
	int c;

	memset(o, 0, sizeof(*o));
	parse_addr("--from", DEFAULT_FROM, 0, &o->from);
	o->samples = DEFAULT_SAMPLES;
	o->sid = DEFAULT_SID;
	o->source = "ramp";
	o->seed = 1;

	/* Leading ':': a missing value is ':', an unknown option '?' */
	opterr = 0;
	while ((c = getopt_long(argc, argv, ":", longopts, NULL)) != -1) {
		name = optind > 1 ? argv[optind - 1] : "";
		switch (c) {
		case OPT_TO:
			parse_addr("--to", optarg, 1, &o->to);
			o->have_to = true;
			break;
		case OPT_FROM:
			parse_addr("--from", optarg, 0, &o->from);
			break;
		case OPT_SAMPLES:
			o->samples = parse_u64("--samples", optarg, 1, MAX_SAMPLES);
			break;
		case OPT_SID:
			o->sid = (uint32_t)parse_u64("--sid", optarg, 0, UINT32_MAX);
			break;
		case OPT_PACKETS:
			o->packets = parse_u64("--packets", optarg, 1, UINT64_MAX);
			break;
		case OPT_DURATION:
			o->duration = parse_double("--duration", optarg, 1e-6, 1e9);
			break;
		case OPT_PPS:
			o->pps = parse_u64("--pps", optarg, 1, 1000000000);
			break;
		case OPT_SOURCE:
			o->source = optarg;
			break;
		case OPT_DROP_LIST:
			parse_list("--drop-list", optarg, &o->drop_list);
			break;
		case OPT_DROP_EVERY:
			o->drop_every = parse_u64("--drop-every", optarg, 1, UINT64_MAX);
			break;
		case OPT_DROP_PROB:
			o->drop_prob = parse_double("--drop-prob", optarg, 0.0, 1.0);
			break;
		case OPT_SEED:
			o->seed = parse_u64("--seed", optarg, 0, UINT64_MAX);
			break;
		case OPT_REORDER:
			o->reorder = parse_u64("--reorder-window", optarg, 2, UINT64_MAX);
			break;
		case OPT_DUP_EVERY:
			o->dup_every = parse_u64("--dup-every", optarg, 1, UINT64_MAX);
			break;
		case OPT_BURST_GAP:
			o->burst_gap_ms = parse_u64("--burst-gap-ms", optarg, 0, 86400000);
			break;
		case OPT_JSON:
			o->json = optarg;
			break;
		case OPT_HELP:
			usage(stdout);
			exit(0);
		case ':':
			usage_error("%s needs a value", name);
			break;
		default:
			usage_error("unknown option '%s'", name);
		}
	}

	if (optind < argc)
		usage_error("unexpected argument '%s'", argv[optind]);
	if (!o->have_to)
		usage_error("--to is required");
	if (!o->packets == !(o->duration > 0))
		usage_error("give exactly one of --packets and --duration");
}

static int source_open(const char *name, struct source *s)
{
	uint8_t *p;
	size_t n;
	FILE *f;

	memset(s, 0, sizeof(*s));
	if (!strcmp(name, "ramp"))
		return 0;

	f = fopen(name, "rb");
	if (!f) {
		fprintf(stderr, "vrt_sender: %s: %s\n", name, strerror(errno));
		return -1;
	}

	for (;;) {
		p = realloc(s->data, s->len + 65536);
		if (!p) {
			fprintf(stderr, "vrt_sender: %s: out of memory\n", name);
			fclose(f);
			return -1;
		}
		s->data = p;
		n = fread(s->data + s->len, 1, 65536, f);
		s->len += n;
		if (n < 65536)
			break;
	}

	if (ferror(f) || !s->len) {
		fprintf(stderr, "vrt_sender: %s: %s\n", name, ferror(f) ? "read error" : "empty");
		fclose(f);
		return -1;
	}
	fclose(f);

	return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static size_t packet_len(const struct opts *o)
{
	return PROLOGUE_BYTES + (size_t)o->samples * 4;
}

/* The datagram of packet <idx> */
static void build(const struct opts *o, const struct source *s, uint64_t idx, uint8_t *pkt)
{
	size_t len = packet_len(o), plen = len - PROLOGUE_BYTES, off, n, k;
	uint64_t frac = idx * o->samples, smp;
	uint8_t *payload = pkt + PROLOGUE_BYTES;

	/* IF data with Stream ID, C, TSI UTC, TSF sample count */
	put32(pkt, (1u << 28) | (1u << 27) | (1u << 22) | (1u << 20) |
					((uint32_t)(idx & 0xF) << 16) | (uint32_t)(len / 4));
	put32(pkt + 4, o->sid);
	put32(pkt + 8, OUI);
	put32(pkt + 12, PACKET_CLASS); /* information class 0 */
	put32(pkt + 16, TSI_SECS);
	put32(pkt + 20, (uint32_t)(frac >> 32));
	put32(pkt + 24, (uint32_t)frac);

	if (!s->data) {
		for (k = 0; k < o->samples; k++) {
			smp = frac + k;
			put16(payload + 4 * k, (uint16_t)(smp & 0x7FFF));
			put16(payload + 4 * k + 2, (uint16_t)((0x8000 - smp) & 0xFFFF));
		}
		return;
	}

	off = (size_t)(((idx % s->len) * (plen % s->len)) % s->len);
	for (k = 0; k < plen; k += n) {
		n = s->len - off < plen - k ? s->len - off : plen - k;
		memcpy(payload + k, s->data + off, n);
		off = (off + n) % s->len;
	}
}

static uint64_t mix64(uint64_t x)
{
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;

	return x ^ (x >> 31);
}

static bool dropped(const struct opts *o, uint64_t idx)
{
	double u;

	if (o->drop_list.len &&
			bsearch(&idx, o->drop_list.v, o->drop_list.len, sizeof(idx), cmp_u64))
		return true;
	if (o->drop_every && (idx + 1) % o->drop_every == 0)
		return true;
	if (o->drop_prob > 0) {
		/* Uniform in [0, 1), depends on the seed and the index only */
		u = (double)(mix64(o->seed ^ mix64(idx)) >> 11) * (1.0 / 9007199254740992.0);
		return u < o->drop_prob;
	}

	return false;
}

static double secs_since(const struct timespec *t0)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (double)(now.tv_sec - t0->tv_sec) + (double)(now.tv_nsec - t0->tv_nsec) / 1e9;
}

static bool gen_more_indices(const struct gen *g)
{
	if (g->o->packets)
		return g->next < g->o->packets;

	return g->next < UINT64_MAX && secs_since(&g->start) < g->o->duration;
}

static int gen_push(struct gen *g, uint64_t idx)
{
	g->pend[g->nb_pend++] = idx;

	if (g->o->dup_every && (idx + 1) % g->o->dup_every == 0) {
		g->pend[g->nb_pend++] = idx;
		return list_add(&g->duplicated, idx);
	}

	return 0;
}

/* Queue the next packet (or reordered pair) to send. 1: queued, 0: no
 * more, < 0: error */
static int gen_refill(struct gen *g)
{
	const struct opts *o = g->o;
	uint64_t idx;
	int ret;

	g->nb_pend = 0;
	g->pos = 0;

	while (gen_more_indices(g)) {
		idx = g->next++;
		if (dropped(o, idx)) {
			ret = list_add(&g->dropped, idx);
			if (ret)
				return ret;
			continue;
		}

		if (o->reorder && idx % o->reorder == o->reorder - 1 &&
				(!o->packets || idx + 1 < o->packets) && !dropped(o, idx + 1)) {
			g->next++;
			ret = gen_push(g, idx + 1);
			if (!ret)
				ret = gen_push(g, idx);
			if (!ret)
				ret = list_add(&g->reordered, idx);
			return ret ? ret : 1;
		}

		ret = gen_push(g, idx);
		return ret ? ret : 1;
	}

	return 0;
}

/* 1 with the next index in *idx, 0 when done, < 0 on error */
static int gen_next(struct gen *g, uint64_t *idx)
{
	int ret;

	if (g->pos == g->nb_pend) {
		ret = gen_refill(g);
		if (ret < 0)
			fprintf(stderr, "vrt_sender: out of memory\n");
		if (ret <= 0)
			return ret;
	}

	*idx = g->pend[g->pos++];

	return 1;
}

static void ts_add_ns(struct timespec *ts, uint64_t ns)
{
	ns += (uint64_t)ts->tv_nsec;
	ts->tv_sec += (time_t)(ns / 1000000000u);
	ts->tv_nsec = (long)(ns % 1000000000u);
}

static bool ts_after(const struct timespec *a, const struct timespec *b)
{
	return a->tv_sec > b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void sleep_until(const struct timespec *t)
{
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL) == EINTR)
		;
}

static int open_socket(const struct opts *o)
{
	int fd, one = 1;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("vrt_sender: socket");
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
			bind(fd, (const struct sockaddr *)&o->from, sizeof(o->from))) {
		perror("vrt_sender: bind --from");
		close(fd);
		return -1;
	}

	return fd;
}

static int send_batch(int fd, struct mmsghdr *msgs, unsigned int nb)
{
	unsigned int done = 0;
	int ret;

	while (done < nb) {
		ret = sendmmsg(fd, msgs + done, nb - done, 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("vrt_sender: sendmmsg");
			return -1;
		}
		done += (unsigned int)ret;
	}

	return 0;
}

/*
 * Send everything the generator yields: batches of the datagrams that are
 * due, up to BATCH, a pause after every BATCH datagrams with --burst-gap-ms.
 * *sent counts the datagrams sent.
 */
static int run(int fd, const struct opts *o, const struct source *s, struct gen *g, uint64_t *sent)
{
	size_t len = packet_len(o);
	struct mmsghdr msgs[BATCH];
	struct iovec iov[BATCH];
	struct timespec t0, due, now;
	uint64_t idx = 0, k = 0;
	bool done = false, have = false, gap;
	unsigned int nb, i;
	uint8_t *slots;
	int ret = 0;

	slots = malloc(len * BATCH);
	if (!slots) {
		fprintf(stderr, "vrt_sender: out of memory\n");
		return -1;
	}

	memset(msgs, 0, sizeof(msgs));
	for (i = 0; i < BATCH; i++) {
		iov[i].iov_base = slots + i * len;
		iov[i].iov_len = len;
		msgs[i].msg_hdr.msg_name = (void *)&o->to;
		msgs[i].msg_hdr.msg_namelen = sizeof(o->to);
		msgs[i].msg_hdr.msg_iov = &iov[i];
		msgs[i].msg_hdr.msg_iovlen = 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	g->start = t0;

	while (!done && ret >= 0) {
		nb = 0;
		gap = false;

		while (nb < BATCH) {
			if (!have) {
				ret = gen_next(g, &idx);
				if (ret <= 0) {
					done = true;
					break;
				}
				have = true;
			}

			if (o->pps) {
				/* Datagram k is due k/pps after t0 */
				due = t0;
				ts_add_ns(&due, k / o->pps * 1000000000u +
								k % o->pps * 1000000000u / o->pps);
				clock_gettime(CLOCK_MONOTONIC, &now);
				if (ts_after(&due, &now)) {
					if (nb)
						break; /* send what is due first */
					sleep_until(&due);
				}
			}

			build(o, s, idx, slots + nb * len);
			have = false;
			nb++;
			k++;
			(*sent)++;

			if (o->burst_gap_ms && *sent % BATCH == 0) {
				gap = true;
				break;
			}
		}

		if (ret < 0)
			break;

		if (nb && send_batch(fd, msgs, nb)) {
			ret = -1;
			break;
		}

		/* Pause only if something follows */
		if (gap && !have) {
			ret = gen_next(g, &idx);
			have = ret > 0;
			done = !ret;
		}
		if (gap && have) {
			clock_gettime(CLOCK_MONOTONIC, &due);
			ts_add_ns(&due, o->burst_gap_ms * 1000000u);
			sleep_until(&due);
			/* Pacing restarts after the gap */
			clock_gettime(CLOCK_MONOTONIC, &t0);
			k = 0;
		}
	}

	free(slots);

	return ret < 0 ? -1 : 0;
}

static void write_list(FILE *f, const struct u64_list *l)
{
	size_t i;

	fputc('[', f);
	for (i = 0; i < l->len; i++)
		fprintf(f, "%s%" PRIu64, i ? ", " : "", l->v[i]);
	fputc(']', f);
}

static int write_oracle(const char *path, const struct gen *g, uint64_t sent)
{
	FILE *f = fopen(path, "w");
	size_t i;

	if (!f) {
		fprintf(stderr, "vrt_sender: %s: %s\n", path, strerror(errno));
		return -1;
	}

	fprintf(f, "{\n  \"sent\": %" PRIu64 ",\n  \"dropped\": ", sent);
	write_list(f, &g->dropped);
	fprintf(f, ",\n  \"reordered\": [");
	for (i = 0; i < g->reordered.len; i++)
		fprintf(f, "%s[%" PRIu64 ", %" PRIu64 "]", i ? ", " : "", g->reordered.v[i],
				g->reordered.v[i] + 1);
	fprintf(f, "],\n  \"duplicated\": ");
	write_list(f, &g->duplicated);
	fprintf(f, "\n}\n");

	if (fclose(f)) {
		fprintf(stderr, "vrt_sender: %s: %s\n", path, strerror(errno));
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct source src;
	struct gen g;
	struct opts o;
	uint64_t sent = 0;
	int fd, ret;

	parse_opts(argc, argv, &o);

	if (source_open(o.source, &src))
		return 1;

	fd = open_socket(&o);
	if (fd < 0)
		return 1;

	memset(&g, 0, sizeof(g));
	g.o = &o;

	ret = run(fd, &o, &src, &g, &sent);
	close(fd);

	if (o.json && write_oracle(o.json, &g, sent))
		ret = -1;

	free(src.data);
	free(o.drop_list.v);
	free(g.dropped.v);
	free(g.reordered.v);
	free(g.duplicated.v);

	return ret ? 1 : 0;
}
