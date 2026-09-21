/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/*
 * CPU side — Zynq dual-ring offload host (throughput / demo).
 *
 * Geometry is runtime-configurable (same idea as rb_config_t):
 *
 *   -c CAP      capacity (power of 2, default 32, max 256)
 *   -z SLOTS    slot count (default = capacity, max 256)
 *   -S BYTES    slot_size (default auto from payload, max 8192)
 *   -s BYTES    payload size (default 64)
 *   -t SECS     timed run (default 5; 0 with -n for count mode)
 *   -n N        publish N messages then stop (overrides -t)
 *   -m wait|drop
 *   -d US       min µs between attempts (0 = max rate)
 *   -v          verbose (seq only)
 *   -D          dump packets: green ingress (send) + red egress (recv)
 *               with high-res timestamps and per-packet latency deltas
 *   -L          collect latency samples and print percentile statistics
 *               without dumping packets (recommended for benchmarks)
 */
#include "hw_port.h"
#include "common.h"
#include "host_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

struct zo_shared *g_zo;

/* Packet dump state (only allocated / used when -D is set) */
#define DUMP_TS_SLOTS  4096u
static int g_dump;
static int g_latency;
static int g_roundtrip;
static uint64_t *g_send_ns;          /* indexed by seq % DUMP_TS_SLOTS */
static uint64_t *g_lat_samples_ns;
static size_t g_lat_samples_cap;
static uint64_t g_lat_sum_ns;
static uint64_t g_lat_min_ns = UINT64_MAX;
static uint64_t g_lat_max_ns;
static uint64_t g_rt_compute_sum_ns;
static uint64_t g_rt_compute_min_ns = UINT64_MAX;
static uint64_t g_rt_compute_max_ns;
static uint64_t g_rt_compute_samples;
static uint64_t g_lat_count;

static int latency_record(uint64_t delta)
{
    if (g_lat_count == g_lat_samples_cap) {
        size_t new_cap = g_lat_samples_cap ? g_lat_samples_cap * 2u : 4096u;
        uint64_t *p = (uint64_t *)realloc(g_lat_samples_ns,
                                          new_cap * sizeof(*p));
        if (!p)
            return -1;
        g_lat_samples_ns = p;
        g_lat_samples_cap = new_cap;
    }
    g_lat_samples_ns[g_lat_count++] = delta;
    g_lat_sum_ns += delta;
    if (delta < g_lat_min_ns)
        g_lat_min_ns = delta;
    if (delta > g_lat_max_ns)
        g_lat_max_ns = delta;
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t latency_percentile(double pct)
{
    if (g_lat_count == 0)
        return 0;
    size_t rank = (size_t)((pct / 100.0) * (double)g_lat_count);
    if (rank == 0)
        rank = 1;
    if (rank > g_lat_count)
        rank = g_lat_count;
    return g_lat_samples_ns[rank - 1];
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double now_sec(void)
{
    return (double)now_ns() * 1e-9;
}

static void sleep_us(long us)
{
    if (us <= 0)
        return;
    struct timespec ts = { .tv_sec = us / 1000000L,
                           .tv_nsec = (us % 1000000L) * 1000L };
    nanosleep(&ts, NULL);
}

static int is_pow2(uint32_t v)
{
    return v >= 2u && (v & (v - 1u)) == 0u;
}

/* Pretty-print a buffer: printable as string, otherwise hex */
static void dump_bytes(const void *data, uint32_t len, FILE *out)
{
    const unsigned char *p = (const unsigned char *)data;
    int printable = 1;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] < 0x20 || p[i] > 0x7e) {
            if (p[i] != '\0' || i + 1 != len) {
                printable = 0;
                break;
            }
        }
    }
    if (printable && len > 0) {
        fprintf(out, "\"");
        for (uint32_t i = 0; i < len; i++) {
            if (p[i] == '\0')
                break;
            fputc(p[i], out);
        }
        fprintf(out, "\"");
        return;
    }
    fprintf(out, "[%u bytes] ", len);
    uint32_t show = len > 64u ? 64u : len;
    for (uint32_t i = 0; i < show; i++)
        fprintf(out, "%02x%s", p[i], (i + 1 < show) ? " " : "");
    if (len > show)
        fprintf(out, " ...");
}

static void dump_ingress(uint32_t seq, const void *payload, uint32_t len, uint64_t ts_ns)
{
    /* green */
    printf("\033[32m");
    printf("[INGRESS  %llu.%09llu s] seq=%u  ",
           (unsigned long long)(ts_ns / 1000000000ull),
           (unsigned long long)(ts_ns % 1000000000ull),
           seq);
    dump_bytes(payload, len, stdout);
    printf("\033[0m\n");
    fflush(stdout);
}

static void dump_egress(const struct zo_result *r, uint32_t len, uint64_t ts_ns,
                        uint64_t send_ns)
{
    uint64_t delta = (send_ns && ts_ns >= send_ns) ? (ts_ns - send_ns) : 0;
    /* red */
    printf("\033[31m");
    printf("[EGRESS   %llu.%09llu s] seq=%u  hash=0x%08x in_len=%u tag=%.4s",
           (unsigned long long)(ts_ns / 1000000000ull),
           (unsigned long long)(ts_ns % 1000000000ull),
           r->seq, r->hash, r->in_len, r->tag);
    if (send_ns) {
        printf("  Δ=%llu ns (%.3f µs)",
               (unsigned long long)delta, (double)delta / 1000.0);
        (void)latency_record(delta);
    }
    printf("\033[0m\n");
    if (len > sizeof(struct zo_result)) {
        printf("\033[31m  (extra %u bytes) ", len - (uint32_t)sizeof(struct zo_result));
        dump_bytes((const char *)r + sizeof(struct zo_result),
                   len - (uint32_t)sizeof(struct zo_result), stdout);
        printf("\033[0m\n");
    }
    fflush(stdout);
}

static struct zo_shared *map_shared(int create)
{
    int fd = shm_open(ZO_SHM_NAME, O_RDWR | (create ? O_CREAT : 0), 0666);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }
    if (create && ftruncate(fd, (off_t)sizeof(struct zo_shared)) != 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, sizeof(struct zo_shared),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return (struct zo_shared *)p;
}

static int init_rings(struct zo_shared *s, uint32_t cap, uint32_t slots,
                      uint32_t slot_size)
{
    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = cap;
    cfg.slots     = slots;
    cfg.slot_size = slot_size;
    cfg.limit     = cap;

    size_t need = (size_t)slots * slot_size;
    if (need > sizeof s->scratch_a) {
        fprintf(stderr, "scratch overflow: need %zu max %zu\n",
                need, sizeof s->scratch_a);
        return -1;
    }

    if (rb_init(zo_ring_a(s), &cfg, s->scratch_a, sizeof s->scratch_a) != RB_OK) {
        fprintf(stderr, "rb_init(ring_a) failed\n");
        return -1;
    }
    if (rb_init(zo_ring_b(s), &cfg, s->scratch_b, sizeof s->scratch_b) != RB_OK) {
        fprintf(stderr, "rb_init(ring_b) failed\n");
        return -1;
    }

    s->bell.cpu_to_fpga = 0;
    s->bell.fpga_to_cpu = 0;
    s->bell.seq_in = 0;
    s->bell.seq_out = 0;

    s->cfg.capacity  = cap;
    s->cfg.slots     = slots;
    s->cfg.slot_size = slot_size;
    s->cfg.magic     = ZO_CFG_MAGIC;
    __sync_synchronize();
    s->cfg.ready     = 1;
    return 0;
}

static const char *sample_json[] = {
    "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"rig/1\"]}",
    "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"user\",\"x\"]}",
    "{\"id\":3,\"method\":\"mining.submit\",\"params\":[\"user\",\"job\",\"0000\"]}",
    "{\"id\":4,\"result\":true,\"error\":null}",
    "{\"id\":5,\"method\":\"mining.notify\",\"params\":[\"job\",\"prev\",\"cb\"]}",
};

static int publish_payload(rb_t *ring, const void *payload, uint32_t want,
                           int drop_mode, uint64_t *full_hits)
{
    uint32_t idx = 0, cap = 0;
    void *w = NULL;
    int spins = 0;

    for (;;) {
        rb_err_t e = rb_acquire(ring, want, &idx, &w, &cap);
        if (e == RB_OK)
            break;
        if (e == RB_ERR_FULL) {
            if (full_hits)
                (*full_hits)++;
            if (drop_mode)
                return 1;
            if ((++spins & 0x3ff) == 0) {
                for (volatile int i = 0; i < 16; i++)
                    ;
            }
            continue;
        }
        fprintf(stderr, "rb_acquire failed (%d)\n", (int)e);
        return -1;
    }

    uint32_t len = want <= cap ? want : cap;
    memcpy(w, payload, len);
    if (rb_publish(ring, idx, len) != RB_OK) {
        fprintf(stderr, "rb_publish failed\n");
        return -1;
    }
    return 0;
}

/* Drain with optional packet dump + latency */
static int drain_results(rb_t *ring, int expected)
{
    int got = 0, idle = 0;
    while (got < expected && idle < 100000000) {
        uint32_t idx = 0, len = 0;
        const void *obj = NULL;
        bool trunc = false;
        if (rb_consume(ring, &idx, &obj, &len, &trunc) != RB_OK) {
            if (g_zo->bell.fpga_to_cpu)
                g_zo->bell.fpga_to_cpu = 0;
            idle++;
            continue;
        }
        idle = 0;
        if (len >= sizeof(struct zo_result)) {
            got++;
            if (g_dump || g_latency) {
                const struct zo_result *r = (const struct zo_result *)obj;
                uint64_t ts = now_ns();
                uint64_t send = 0;
                if (g_send_ns)
                    send = g_send_ns[r->seq % DUMP_TS_SLOTS];
                if (g_dump)
                    dump_egress(r, len, ts, send);
                else if (send && ts >= send)
                    (void)latency_record(ts - send);
            }
        }
        rb_release(ring, idx);
    }
    return got;
}

static int run_roundtrip(rb_t *ring, const void *payload, uint32_t payload_len,
                         uint64_t limit, double duration, int have_n)
{
    uint64_t completed = 0, full_hits = 0;
    double t_end = t0 + duration;

    for (;;) {
        if (have_n) {
            if (completed >= limit)
                break;
        } else if (now_sec() >= t_end) {
            break;
        }

        uint32_t idx = 0, len = 0;
        const void *obj = NULL;
        bool trunc = false;
        uint32_t seq = ++g_zo->bell.seq_in;
        uint64_t start = now_ns();

        if (publish_payload(ring, payload, payload_len, 0, &full_hits) != 0)
            return -1;

        for (;;) {
            if (rb_consume(zo_ring_b(g_zo), &idx, &obj, &len, &trunc) == RB_OK)
                break;
            if (g_zo->bell.fpga_to_cpu)
                g_zo->bell.fpga_to_cpu = 0;
        }

        uint64_t end = now_ns();
        const struct zo_result *r = (const struct zo_result *)obj;
        if (len < sizeof(*r) || r->seq != seq) {
            fprintf(stderr, "roundtrip: bad response seq=%u expected=%u len=%u\n",
                    len >= sizeof(*r) ? r->seq : 0u, seq, len);
            rb_release(zo_ring_b(g_zo), idx);
            return -1;
        }

        (void)latency_record(end - start);
        g_rt_compute_sum_ns += r->compute_ns;
        if (r->compute_ns < g_rt_compute_min_ns)
            g_rt_compute_min_ns = r->compute_ns;
        if (r->compute_ns > g_rt_compute_max_ns)
            g_rt_compute_max_ns = r->compute_ns;
        g_rt_compute_samples++;

        rb_release(zo_ring_b(g_zo), idx);
        completed++;
    }

    double elapsed = now_sec() - t0;
    printf("roundtrip done: completed=%llu full_hits=%llu t=%.3fs\n",
           (unsigned long long)completed, (unsigned long long)full_hits, elapsed);
    printf("  serialized RTT rate=%.0f transactions/s\n",
           elapsed > 0.0 ? (double)completed / elapsed : 0.0);
    return completed == (uint64_t)(int)completed ? (int)completed : -1;
}

static void usage(const char *prog)
{
    printf(
        "usage: %s [options]\n"
        "  -c CAP      capacity (power of 2, default %u, max %u)\n"
        "  -z SLOTS    slot count (default = capacity, max %u)\n"
        "  -S BYTES    slot_size (default auto from -s, max %u)\n"
        "  -s BYTES    payload size (default %u)\n"
        "  -t SECS     timed run duration (default 5)\n"
        "  -n N        publish exactly N messages (overrides -t;\n"
        "              N<=5 uses sample JSON demo frames)\n"
        "  -m MODE     wait | drop  (default wait)\n"
        "  -d US       min µs between attempts (0 = max rate)\n"
        "  -v          verbose (print published seq)\n"
        "  -D          dump packets with timestamps (green=ingress,\n"
        "              red=egress) and per-packet latency Δ\n"
        "  -h          help\n"
        "\n"
        "Prints rates and theoretical no-drop msg/sec at the end.\n"
        "Use -D with -n N or -d US (low rate) — high-rate dumps flood the terminal.\n",
        prog,
        ZO_DEFAULT_CAPACITY, ZO_MAX_CAPACITY,
        ZO_MAX_SLOTS, ZO_MAX_SLOT_SIZE, ZO_DEFAULT_PAYLOAD);
}

int main(int argc, char **argv)
{
    double duration = 5.0;
    uint64_t total = 0;
    uint32_t capacity = ZO_DEFAULT_CAPACITY;
    uint32_t slots = 0;
    uint32_t slot_size = 0;
    uint32_t msg_size = ZO_DEFAULT_PAYLOAD;
    int drop_mode = 0;
    int verbose = 0;
    int have_n = 0;
    long pace_us = 0;
    struct host_snapshot host_start, host_end;
    double t0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            capacity = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-z") && i + 1 < argc)
            slots = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-S") && i + 1 < argc)
            slot_size = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            msg_size = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            duration = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            total = strtoull(argv[++i], NULL, 0);
            have_n = 1;
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            pace_us = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            ++i;
            if (!strcmp(argv[i], "drop"))
                drop_mode = 1;
            else if (!strcmp(argv[i], "wait"))
                drop_mode = 0;
            else {
                fprintf(stderr, "unknown mode: %s\n", argv[i]);
                return 1;
            }
        } else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-D"))
            g_dump = 1;
        else if (!strcmp(argv[i], "-L"))
            g_latency = 1;
        else if (!strcmp(argv[i], "-R"))
            g_roundtrip = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!slots)
        slots = capacity;
    if (!slot_size) {
        slot_size = msg_size + 64u;
        if (slot_size < 128u)
            slot_size = 128u;
        slot_size = (slot_size + 63u) & ~63u;
    }

    if (!is_pow2(capacity) || capacity > ZO_MAX_CAPACITY) {
        fprintf(stderr, "capacity %u must be power-of-2 in [2, %u]\n",
                capacity, ZO_MAX_CAPACITY);
        return 1;
    }
    if (slots < 1u || slots > ZO_MAX_SLOTS) {
        fprintf(stderr, "slots %u out of range [1, %u]\n", slots, ZO_MAX_SLOTS);
        return 1;
    }
    if (slot_size < 16u || slot_size > ZO_MAX_SLOT_SIZE) {
        fprintf(stderr, "slot_size %u out of range [16, %u]\n",
                slot_size, ZO_MAX_SLOT_SIZE);
        return 1;
    }
    if (msg_size < 4u || msg_size + 8u > slot_size) {
        fprintf(stderr, "payload %u does not fit in slot_size %u\n",
                msg_size, slot_size);
        return 1;
    }

    int demo = (have_n && total <= 5);
    if (g_roundtrip)
        demo = 0; /* RTT benchmark always uses the fixed binary payload. */

    if (g_dump || g_latency || g_roundtrip) {
        g_send_ns = (uint64_t *)calloc(DUMP_TS_SLOTS, sizeof(uint64_t));
        if (!g_send_ns) {
            perror("calloc send_ts");
            return 1;
        }
    }


    printf("zynq_offload cpu_host\n");
    printf("  capacity=%u slots=%u slot_size=%u payload=%u mode=%s pace_us=%ld dump=%s\n",
           capacity, slots, slot_size, msg_size,
           drop_mode ? "drop" : "wait", pace_us,
           g_dump ? "on" : "off");
    printf("  latency=%s  roundtrip=%s\n",
           g_latency ? "on" : "off", g_roundtrip ? "on" : "off");
    if (have_n)
        printf("  limit=%llu\n", (unsigned long long)total);
    else
        printf("  duration=%.1fs\n", duration);

    /*
     * Do not shm_unlink() here.  Another process (fpga_stub or, on the
     * target, the FPGA-side mapping) may already have this object mapped.
     * Unlinking removes the name and lets shm_open(O_CREAT) create a new
     * object, leaving the peer attached to the old one.
     *
     * Reusing the named object is safe because init_rings() completely
     * reinitializes the shared ring state before setting cfg.ready = 1.
     */
    g_zo = map_shared(1);
    if (!g_zo)
        return 1;
    if (init_rings(g_zo, capacity, slots, slot_size) != 0)
        return 1;

    char *payload = NULL;
    if (!demo) {
        payload = (char *)malloc(msg_size);
        if (!payload) {
            perror("malloc");
            return 1;
        }
        memset(payload, 0xA5, msg_size);
        memcpy(payload, "ZYNQ", 4);
    }

    sleep_us(150000);

    host_print_identity();
    t0 = now_sec();
    host_snapshot_take(&host_start);

    if (g_roundtrip) {
        int rt = run_roundtrip(zo_ring_a(g_zo), payload, msg_size,
                               total, duration, have_n);
        if (rt < 0)
            return 1;

        if (g_lat_count > 0) {
            qsort(g_lat_samples_ns, g_lat_count,
                  sizeof(*g_lat_samples_ns), cmp_u64);
            printf("\n=== Round-trip latency (CPU -> FPGA -> CPU) ===\n");
            printf("samples=%llu  min=%.3f us  p10=%.3f us  p50=%.3f us\n",
                   (unsigned long long)g_lat_count,
                   (double)g_lat_min_ns / 1000.0,
                   (double)latency_percentile(10.0) / 1000.0,
                   (double)latency_percentile(50.0) / 1000.0);
            printf("p90=%.3f us  p99=%.3f us  p99.9=%.3f us\n",
                   (double)latency_percentile(90.0) / 1000.0,
                   (double)latency_percentile(99.0) / 1000.0,
                   (double)latency_percentile(99.9) / 1000.0);
            printf("avg=%.3f us  max=%.3f us\n",
                   (double)g_lat_sum_ns / (double)g_lat_count / 1000.0,
                   (double)g_lat_max_ns / 1000.0);

            printf("\n=== FPGA stub compute (FNV hash only) ===\n");
            printf("samples=%llu  min=%.3f us  avg=%.3f us  max=%.3f us\n",
                   (unsigned long long)g_rt_compute_samples,
                   (double)g_rt_compute_min_ns / 1000.0,
                   (double)g_rt_compute_sum_ns /
                       (double)g_rt_compute_samples / 1000.0,
                   (double)g_rt_compute_max_ns / 1000.0);
            printf("===========================================\n");
        }

        g_zo->cfg.ready = 0;
        free(payload);
        host_snapshot_take(&host_end);
        host_print_delta(&host_start, &host_end, now_sec() - t0);
        free(g_send_ns);
        free(g_lat_samples_ns);
        return 0;
    }

    t0 = now_sec();
    double t_end = t0 + duration;
    uint64_t published = 0, dropped = 0, full_hits = 0, attempted = 0;
    int got_live = 0;

    for (;;) {
        if (have_n) {
            if (attempted >= total)
                break;
        } else if (now_sec() >= t_end) {
            break;
        }

        /* concurrent drain of egress (ring B) */
        {
            uint32_t idx = 0, len = 0;
            const void *obj = NULL;
            bool trunc = false;
            while (rb_consume(zo_ring_b(g_zo), &idx, &obj, &len, &trunc) == RB_OK) {
                if (len >= sizeof(struct zo_result)) {
                    got_live++;
                    if (g_dump || g_latency) {
                        const struct zo_result *r = (const struct zo_result *)obj;
                        uint64_t ts = now_ns();
                        uint64_t send = 0;
                        if (g_send_ns)
                            send = g_send_ns[r->seq % DUMP_TS_SLOTS];
                        if (g_dump)
                            dump_egress(r, len, ts, send);
                        else if (send && ts >= send)
                            (void)latency_record(ts - send);
                    }
                }
                rb_release(zo_ring_b(g_zo), idx);
            }
            if (g_zo->bell.fpga_to_cpu)
                g_zo->bell.fpga_to_cpu = 0;
        }

        uint32_t seq = ++g_zo->bell.seq_in;
        int rc;
        const void *pub_data = NULL;
        uint32_t pub_len = 0;
        if (demo) {
            const char *json = sample_json[attempted % 5];
            uint32_t want = (uint32_t)strlen(json) + 1u;
            pub_data = json;
            pub_len = want;
            rc = publish_payload(zo_ring_a(g_zo), json, want, drop_mode, &full_hits);
        } else {
            memcpy(payload + 4, &seq, sizeof seq);
            pub_data = payload;
            pub_len = msg_size;
            rc = publish_payload(zo_ring_a(g_zo), payload, msg_size,
                                 drop_mode, &full_hits);
        }
        attempted++;
        if (rc < 0)
            return 1;
        if (rc == 1)
            dropped++;
        else {
            published++;
            uint64_t ts = now_ns();
            if (g_send_ns)
                g_send_ns[seq % DUMP_TS_SLOTS] = ts;
            if (g_dump)
                dump_ingress(seq, pub_data, pub_len, ts);
            else if (verbose)
                printf("CPU published seq=%u\n", seq);
        }
        if (pace_us > 0)
            sleep_us(pace_us);
    }

    double t_pub = now_sec() - t0;
    printf("publish done: published=%llu dropped=%llu full_hits=%llu "
           "attempted=%llu t=%.3fs\n",
           (unsigned long long)published,
           (unsigned long long)dropped,
           (unsigned long long)full_hits,
           (unsigned long long)attempted,
           t_pub);
    printf("  attempted=%.0f msg/s  published=%.0f msg/s\n",
           t_pub > 0 ? (double)attempted / t_pub : 0.0,
           t_pub > 0 ? (double)published / t_pub : 0.0);

    int remaining = (int)published - got_live;
    if (remaining < 0)
        remaining = 0;
    int got_extra = drain_results(zo_ring_b(g_zo), remaining);
    int got = got_live + got_extra;
    double t_total = now_sec() - t0;
    double consumer_rate = (got > 0 && t_total > 0.0) ? (double)got / t_total : 0.0;

    printf("done: got %d / %llu (live=%d final=%d) total=%.3fs\n",
           got, (unsigned long long)published, got_live, got_extra, t_total);

    if ((g_dump || g_latency) && g_lat_count > 0) {
        if (g_latency && !g_dump)
            qsort(g_lat_samples_ns, g_lat_count, sizeof(*g_lat_samples_ns), cmp_u64);
        printf("\n=== Latency (ingress → egress) ===\n");
        printf("samples=%llu  min=%.3f µs  p10=%.3f µs  p50=%.3f µs\n",
               (unsigned long long)g_lat_count,
               (double)g_lat_min_ns / 1000.0,
               (double)latency_percentile(10.0) / 1000.0,
               (double)latency_percentile(50.0) / 1000.0);
        printf("p90=%.3f µs  p99=%.3f µs  p99.9=%.3f µs\n",
               (double)latency_percentile(90.0) / 1000.0,
               (double)latency_percentile(99.0) / 1000.0,
               (double)latency_percentile(99.9) / 1000.0);
        printf("avg=%.3f µs  max=%.3f µs\n",
               (double)g_lat_sum_ns / (double)g_lat_count / 1000.0,
               (double)g_lat_max_ns / 1000.0);
        printf("==================================\n");
    }

    printf("\n=== Theoretical no-drop rate ===\n");
    printf("params: capacity=%u slots=%u slot_size=%u payload=%u mode=%s\n",
           capacity, slots, slot_size, msg_size,
           drop_mode ? "drop" : "wait");
    printf("Observed consumer throughput: %.0f msg/s\n", consumer_rate);
    if (dropped > 0) {
        printf("Drops=%llu — producer outran consumer.\n",
               (unsigned long long)dropped);
        printf("No-drop ceiling under these params ≈ %.0f msg/s.\n",
               consumer_rate);
    } else {
        printf("Drops=0 — sustainable no-drop rate ≈ %.0f msg/s.\n",
               t_pub > 0 ? (double)published / t_pub : consumer_rate);
    }
    printf("================================\n");

    host_snapshot_take(&host_end);
    host_print_delta(&host_start, &host_end, t_total);

    g_zo->cfg.ready = 0;
    free(payload);
    free(g_send_ns);
    free(g_lat_samples_ns);
    return (got == (int)published) ? 0 : 2;
}
