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
 *   -v          verbose
 */
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

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
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
        if (len >= sizeof(struct zo_result))
            got++;
        rb_release(ring, idx);
    }
    return got;
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
        "  -d US       min \u00b5s between attempts (0 = max rate)\n"
        "  -v          verbose\n"
        "  -h          help\n"
        "\n"
        "Prints rates and theoretical no-drop msg/sec at the end.\n",
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

    printf("zynq_offload cpu_host\n");
    printf("  capacity=%u slots=%u slot_size=%u payload=%u mode=%s pace_us=%ld\n",
           capacity, slots, slot_size, msg_size,
           drop_mode ? "drop" : "wait", pace_us);
    if (have_n)
        printf("  limit=%llu\n", (unsigned long long)total);
    else
        printf("  duration=%.1fs\n", duration);

    shm_unlink(ZO_SHM_NAME);
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

    double t0 = now_sec();
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

        {
            uint32_t idx = 0, len = 0;
            const void *obj = NULL;
            bool trunc = false;
            while (rb_consume(zo_ring_b(g_zo), &idx, &obj, &len, &trunc) == RB_OK) {
                rb_release(zo_ring_b(g_zo), idx);
                got_live++;
            }
            if (g_zo->bell.fpga_to_cpu)
                g_zo->bell.fpga_to_cpu = 0;
        }

        uint32_t seq = ++g_zo->bell.seq_in;
        int rc;
        if (demo) {
            const char *json = sample_json[attempted % 5];
            uint32_t want = (uint32_t)strlen(json) + 1u;
            rc = publish_payload(zo_ring_a(g_zo), json, want, drop_mode, &full_hits);
        } else {
            memcpy(payload + 4, &seq, sizeof seq);
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
            if (verbose)
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

    printf("\n=== Theoretical no-drop rate ===\n");
    printf("params: capacity=%u slots=%u slot_size=%u payload=%u mode=%s\n",
           capacity, slots, slot_size, msg_size,
           drop_mode ? "drop" : "wait");
    printf("Observed consumer throughput: %.0f msg/s\n", consumer_rate);
    if (dropped > 0) {
        printf("Drops=%llu — producer outran consumer.\n",
               (unsigned long long)dropped);
        printf("No-drop ceiling under these params \u2248 %.0f msg/s.\n",
               consumer_rate);
    } else {
        printf("Drops=0 — sustainable no-drop rate \u2248 %.0f msg/s.\n",
               t_pub > 0 ? (double)published / t_pub : consumer_rate);
    }
    printf("================================\n");

    g_zo->cfg.ready = 0;
    free(payload);
    return (got == (int)published) ? 0 : 2;
}
