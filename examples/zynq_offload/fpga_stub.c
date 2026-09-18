/* SPDX-License-Identifier: MIT */
/*
 * Software model of the FPGA side.
 *
 * Quiet by default (end-of-run stats only).  Pass -v for packet dumps.
 * Ring capacity is chosen by the host (-s) at rb_init; this process
 * waits for bell.ready and uses the already-initialized rings.
 *
 * Typical pair:
 *   terminal 1: ./zynq_fpga_stub -t 70
 *   terminal 2: ./zynq_cpu_host  -t 60 -s 256
 */
#define _POSIX_C_SOURCE 200809L

#include "hw_port.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct zo_shared *g_zo;
static int g_verbose;

static void sleep_us(unsigned us)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(us / 1000000u),
        .tv_nsec = (long)((us % 1000000u) * 1000u),
    };
    nanosleep(&ts, NULL);
}

static struct zo_shared *map_shared(void)
{
    for (int i = 0; i < 50; i++) {
        int fd = shm_open(ZO_SHM_NAME, O_RDWR, 0666);
        if (fd >= 0) {
            void *p = mmap(NULL, sizeof(struct zo_shared),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (p != MAP_FAILED)
                return (struct zo_shared *)p;
        }
        sleep_us(100000);
    }
    fprintf(stderr, "fpga_stub: timed out waiting for %s\n", ZO_SHM_NAME);
    return NULL;
}

/* Wait until host has finished rb_init (bell.ready == 1). */
static int wait_ready(struct zo_shared *s, double timeout_s)
{
    double t0 = zo_now_sec();
    while (!s->bell.ready) {
        if (zo_now_sec() - t0 >= timeout_s) {
            fprintf(stderr, "fpga_stub: timed out waiting for host rb_init\n");
            return -1;
        }
        sleep_us(10000);
    }
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("" ::: "memory");
#endif
    return 0;
}

static int parse_u64_field(const char *json, const char *key, uint64_t *out)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_u32_field(const char *json, const char *key, uint32_t *out)
{
    uint64_t v = 0;
    if (parse_u64_field(json, key, &v) != 0)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int parse_str_field(const char *json, const char *key,
                           char *out, size_t outsz)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz)
        out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static int process_one(rb_t *in, rb_t *out, double max_seconds, double t0)
{
    uint32_t idx = 0, len = 0;
    const void *obj = NULL;
    bool trunc = false;

    if (rb_consume(in, &idx, &obj, &len, &trunc) != RB_OK)
        return 0;

    char json_copy[ZO_SLOT_SIZE];
    uint32_t copy_len = len < sizeof json_copy ? len : (uint32_t)sizeof json_copy - 1u;
    memcpy(json_copy, obj, copy_len);
    json_copy[copy_len] = '\0';
    uint32_t plen = (uint32_t)strlen(json_copy);

    uint64_t ts_in = 0;
    uint32_t seq_in = 0;
    char payload[64] = "";
    (void)parse_u64_field(json_copy, "ts", &ts_in);
    (void)parse_u32_field(json_copy, "seq", &seq_in);
    (void)parse_str_field(json_copy, "payload", payload, sizeof payload);

    if (g_verbose) {
        uint64_t ts_local = zo_now_ns();
        int64_t delta_ns = ts_in ? (int64_t)ts_local - (int64_t)ts_in : 0;

        printf(ZO_CLR_GREEN
               "[FPGA R] seq=%u len=%u trunc=%d\n"
               "[FPGA R] msg=%.*s\n"
               "[FPGA R] payload=\"%s\"\n"
               "[FPGA R] ts_in=%llu ts_local=%llu delta_ns=%lld\n"
               ZO_CLR_RESET,
               seq_in, len, (int)trunc,
               (int)plen, json_copy,
               payload,
               (unsigned long long)ts_in,
               (unsigned long long)ts_local,
               (long long)delta_ns);
    }

    uint32_t h = zo_fast_hash(obj, len);
    uint32_t seq_out = ++g_zo->bell.seq_out;
    uint64_t ts_fpga = zo_now_ns();

    uint32_t oidx = 0, ocap = 0;
    void *w = NULL;
    int full_spins = 0;
    for (;;) {
        if (max_seconds > 0.0 && (zo_now_sec() - t0) >= max_seconds) {
            fprintf(stderr, "FPGA: duration expired while waiting for egress space\n");
            rb_release(in, idx);
            return 2;
        }

        rb_err_t e = rb_acquire(out, sizeof(struct zo_result), &oidx, &w, &ocap);
        if (e == RB_OK)
            break;
        if (e == RB_ERR_FULL) {
            if ((++full_spins % 1000) == 0) {
                fprintf(stderr,
                        "FPGA: egress ring full (CPU not draining?) — waiting…\n");
            }
            sleep_us(100);
            continue;
        }
        fprintf(stderr, "FPGA rb_acquire(egress) failed (%d)\n", (int)e);
        rb_release(in, idx);
        return -1;
    }

    struct zo_result *r = (struct zo_result *)w;
    memset(r, 0, sizeof *r);
    r->seq     = seq_in ? seq_in : seq_out;
    r->hash    = h;
    r->in_len  = len;
    r->ts_in   = ts_in;
    r->ts_fpga = ts_fpga;
    memcpy(r->tag, "HASH", 4);
    {
        size_t n = strlen(payload);
        if (n >= sizeof r->payload)
            n = sizeof r->payload - 1u;
        memcpy(r->payload, payload, n);
        r->payload[n] = 0;
    }

    if (rb_publish(out, oidx, (uint32_t)sizeof *r) != RB_OK) {
        fprintf(stderr, "FPGA rb_publish(egress) failed\n");
        rb_abort(out);
        rb_release(in, idx);
        return -1;
    }
    g_zo->bell.fpga_to_cpu = 1u;

    if (g_verbose) {
        printf(ZO_CLR_RED
               "[FPGA W] seq=%u hash=0x%08x in_len=%u tag=%.4s\n"
               "[FPGA W] payload=\"%.60s\"\n"
               "[FPGA W] ts_in=%llu ts_fpga=%llu\n"
               ZO_CLR_RESET,
               r->seq, r->hash, r->in_len, r->tag,
               r->payload,
               (unsigned long long)r->ts_in,
               (unsigned long long)r->ts_fpga);
        zo_print_sep();
    }

    rb_release(in, idx);
    return 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-t <seconds>] [-v] [-h]\n"
            "\n"
            "What this program does\n"
            "  Userspace model of the FPGA (PL) path.  Consumes JSON from\n"
            "  ingress ring A, hashes the payload, publishes to egress ring B.\n"
            "  Ring capacity is set by the host (-s) at rb_init; this process\n"
            "  waits for the shared header ready flag and uses those rings.\n"
            "\n"
            "  Pair with ./zynq_cpu_host.  Shared memory: %s\n"
            "\n"
            "Options\n"
            "  -t <seconds>  wall-clock runtime (recommended).  Without -t,\n"
            "                exit after ~2 s of idle ingress.\n"
            "  -v            verbose: per-packet dumps + periodic rate lines.\n"
            "                Default is quiet (final stats only).\n"
            "  -h            show this help and exit\n"
            "\n"
            "Example\n"
            "  terminal 1: ./zynq_fpga_stub -t 70\n"
            "  terminal 2: ./zynq_cpu_host  -t 60 -s 256\n",
            prog, ZO_SHM_NAME);
}

int main(int argc, char **argv)
{
    double max_seconds = -1.0;
    int c;
    while ((c = getopt(argc, argv, "t:vh")) != -1) {
        switch (c) {
        case 't': {
            char *end = NULL;
            max_seconds = strtod(optarg, &end);
            if (!end || *end != '\0' || !(max_seconds > 0.0)) {
                fprintf(stderr, "%s: invalid -t value '%s'\n", argv[0], optarg);
                return 2;
            }
            break;
        }
        case 'v':
            g_verbose = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    printf("zynq_offload fpga_stub — verbose=%s",
           g_verbose ? "on" : "off (stats at end only)");
    if (max_seconds > 0.0)
        printf("  duration=%.2fs", max_seconds);
    else
        printf("  (idle-exit after ~2s)");
    printf("\n");

    g_zo = map_shared();
    if (!g_zo)
        return 1;

    if (wait_ready(g_zo, 30.0) != 0)
        return 1;

    uint32_t slots = g_zo->bell.slots;
    if (slots < 1u || slots > ZO_MAX_SLOTS) {
        fprintf(stderr, "fpga_stub: bad slots in shared header (%u)\n", slots);
        return 1;
    }
    printf("FPGA stub: host rings ready (slots=%u)\n", slots);

    rb_t *in  = zo_ring_a(g_zo);
    rb_t *out = zo_ring_b(g_zo);

    double t0 = zo_now_sec();
    double t_last_report = t0;

    uint64_t total = 0;
    int idle_rounds = 0;
    const int idle_limit = 100;

    for (;;) {
        double now = zo_now_sec();
        if (max_seconds > 0.0 && (now - t0) >= max_seconds)
            break;

        int n = process_one(in, out, max_seconds, t0);
        if (n == 1) {
            total += 1;
            idle_rounds = 0;
            g_zo->bell.cpu_to_fpga = 0;
        } else if (n == 0) {
            idle_rounds++;
            if (max_seconds < 0.0 && idle_rounds >= idle_limit)
                break;
            sleep_us(20000);
        } else if (n == 2) {
            break;
        } else {
            return 2;
        }

        if (g_verbose) {
            now = zo_now_sec();
            if (now - t_last_report >= 1.0) {
                double elapsed = now - t0;
                double rate = elapsed > 0.0 ? (double)total / elapsed : 0.0;
                printf("[FPGA  ] rate=%.0f pkt/s  processed=%llu  "
                       "queue_A=%u/%u  queue_B=%u/%u\n",
                       rate,
                       (unsigned long long)total,
                       rb_count(in), rb_limit(in),
                       rb_count(out), rb_limit(out));
                t_last_report = now;
            }
        }
    }

    double elapsed = zo_now_sec() - t0;
    double rate = elapsed > 0.0 ? (double)total / elapsed : 0.0;
    printf("FPGA stub done: processed=%llu  rate=%.0f pkt/s  slots=%u  (%.2fs elapsed)\n",
           (unsigned long long)total, rate, slots, elapsed);
    return 0;
}
