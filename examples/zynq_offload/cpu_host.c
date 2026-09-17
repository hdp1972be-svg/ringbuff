/* SPDX-License-Identifier: MIT */
/*
 * CPU side of the Zynq dual-ring offload example.
 *
 * Simulates "NIC / WebSocket JSON arrived" by publishing JSON frames
 * into ring A for a configurable duration, then drains hashed results
 * from ring B.
 *
 * On a real Antminer S9 replace the shm_open path with mmap of the
 * reserved DDR/BRAM window and feed real socket data into the publish
 * loop.
 *
 * Usage:
 *   ./zynq_cpu_host -t <seconds>
 *   -t <seconds>  how long to keep publishing (required, > 0)
 */
#include "hw_port.h"   /* installs RB_HW_* overrides */
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

static double elapsed_seconds(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) +
           (double)(now.tv_nsec - start->tv_nsec) / 1e9;
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

static int init_rings(struct zo_shared *s)
{
    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = ZO_CAPACITY;
    cfg.slots     = ZO_SLOTS;
    cfg.slot_size = ZO_SLOT_SIZE;
    cfg.limit     = ZO_CAPACITY;

    if (rb_init(zo_ring_a(s), &cfg, s->scratch_a, sizeof s->scratch_a) != RB_OK)
        return -1;
    if (rb_init(zo_ring_b(s), &cfg, s->scratch_b, sizeof s->scratch_b) != RB_OK)
        return -1;

    s->bell.cpu_to_fpga = 0;
    s->bell.fpga_to_cpu = 0;
    s->bell.seq_in = 0;
    s->bell.seq_out = 0;
    return 0;
}

/* Fake "WebSocket JSON from the NIC". */
static const char *sample_json[] = {
    "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"rig/1\"]}",
    "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"user\",\"x\"]}",
    "{\"id\":3,\"method\":\"mining.submit\",\"params\":[\"user\",\"job\",\"0000\"]}",
    "{\"id\":4,\"result\":true,\"error\":null}",
    "{\"id\":5,\"method\":\"mining.notify\",\"params\":[\"job\",\"prev\",\"cb\"]}",
};

static int publish_json(rb_t *ring, const char *json, uint32_t seq)
{
    uint32_t idx = 0, cap = 0;
    void *w = NULL;
    uint32_t want = (uint32_t)strlen(json) + 1u; /* include NUL for demo */

    for (;;) {
        rb_err_t e = rb_acquire(ring, want, &idx, &w, &cap);
        if (e == RB_OK)
            break;
        if (e == RB_ERR_FULL) {
            usleep(100);
            continue;
        }
        fprintf(stderr, "rb_acquire failed (%d)\n", (int)e);
        return -1;
    }

    uint32_t len = want <= cap ? want : cap;
    memcpy(w, json, len);
    (void)seq;

    if (rb_publish(ring, idx, len) != RB_OK) {
        fprintf(stderr, "rb_publish failed\n");
        return -1;
    }
    /* RB_HW_FLUSH_SLOT + RB_HW_NOTIFY_DEVICE already ran inside publish. */
    return 0;
}

/* Drain egress results until quiet for quiet_ms, or overall deadline. */
static int drain_results(rb_t *ring, double quiet_s, const struct timespec *deadline_start,
                         double deadline_s)
{
    int got = 0;
    struct timespec last_hit;
    clock_gettime(CLOCK_MONOTONIC, &last_hit);
    int have_hit = 0;

    for (;;) {
        if (elapsed_seconds(deadline_start) >= deadline_s)
            break;

        uint32_t idx = 0, len = 0;
        const void *obj = NULL;
        bool trunc = false;

        if (rb_consume(ring, &idx, &obj, &len, &trunc) != RB_OK) {
            if (g_zo->bell.fpga_to_cpu) {
                g_zo->bell.fpga_to_cpu = 0;
                continue;
            }
            if (have_hit && elapsed_seconds(&last_hit) >= quiet_s)
                break;
            usleep(200);
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &last_hit);
        have_hit = 1;

        if (len < sizeof(struct zo_result)) {
            fprintf(stderr, "short result len=%u\n", len);
            rb_release(ring, idx);
            continue;
        }

        const struct zo_result *r = (const struct zo_result *)obj;
        printf("CPU  result: seq=%u hash=0x%08x in_len=%u tag=%.4s%s\n",
               r->seq, r->hash, r->in_len, r->tag,
               trunc ? " (trunc)" : "");
        /* Extra payload past the fixed header, if any. */
        if (len > sizeof(struct zo_result)) {
            const uint8_t *extra = (const uint8_t *)obj + sizeof(struct zo_result);
            uint32_t elen = len - (uint32_t)sizeof(struct zo_result);
            printf("CPU  result extra (%u B): ", elen);
            for (uint32_t i = 0; i < elen && i < 64u; i++) {
                unsigned char c = extra[i];
                putchar((c >= 32 && c < 127) ? (char)c : '.');
            }
            if (elen > 64u)
                printf("…");
            putchar('\n');
        }
        rb_release(ring, idx);
        got++;
    }
    return got;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -t <seconds>\n"
            "  -t <seconds>  publish duration (required, > 0)\n"
            "\n"
            "Cycles sample WebSocket JSON frames into ring A for the given\n"
            "duration, then drains hashed results from ring B.\n",
            prog);
}

int main(int argc, char **argv)
{
    double seconds = -1.0;
    int c;
    while ((c = getopt(argc, argv, "t:h")) != -1) {
        switch (c) {
        case 't': {
            char *end = NULL;
            seconds = strtod(optarg, &end);
            if (!end || *end != '\0' || !(seconds > 0.0)) {
                fprintf(stderr, "%s: invalid -t value '%s'\n", argv[0], optarg);
                return 2;
            }
            break;
        }
        case 'h':
        default:
            usage(argv[0]);
            return c == 'h' ? 0 : 2;
        }
    }
    if (!(seconds > 0.0)) {
        usage(argv[0]);
        return 2;
    }

    printf("zynq_offload cpu_host — creating shared region (duration=%.2fs)\n",
           seconds);
    g_zo = map_shared(1);
    if (!g_zo)
        return 1;

    /* Sanity-check alignment before rb_init (which also checks). */
    if (((uintptr_t)zo_ring_a(g_zo) % RB_CACHE_LINE) != 0 ||
        ((uintptr_t)zo_ring_b(g_zo) % RB_CACHE_LINE) != 0 ||
        ((uintptr_t)g_zo->scratch_a % RB_CACHE_LINE) != 0 ||
        ((uintptr_t)g_zo->scratch_b % RB_CACHE_LINE) != 0) {
        fprintf(stderr, "shared-region alignment broken (need RB_CACHE_LINE=%u)\n",
                (unsigned)RB_CACHE_LINE);
        return 1;
    }

    if (init_rings(g_zo) != 0) {
        fprintf(stderr, "rb_init failed (check alignment / sizes)\n");
        return 1;
    }

    const int n_samples = (int)(sizeof sample_json / sizeof sample_json[0]);
    printf("publishing sample WS-JSON frames into ingress ring A for %.2fs "
           "(%d unique samples, cycling)\n", seconds, n_samples);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int published = 0;
    int i = 0;

    while (elapsed_seconds(&start) < seconds) {
        const char *json = sample_json[i % n_samples];
        uint32_t seq = ++g_zo->bell.seq_in;
        if (publish_json(zo_ring_a(g_zo), json, seq) != 0)
            return 1;
        printf("CPU  published seq=%u len=%zu json=%s\n",
               seq, strlen(json), json);
        published++;
        i++;
        /* Small pause so a slow stub can keep up during short runs. */
        usleep(500);
    }

    printf("publish window done (%d frames). waiting for FPGA results "
           "on egress ring B …\n", published);

    /* Allow extra time after publish window for the stub to finish. */
    int got = drain_results(zo_ring_b(g_zo), 0.5, &start, seconds + 5.0);
    printf("done: published=%d received=%d\n", published, got);

    /* Leave shm in place so the stub can exit cleanly; user may rm it. */
    return got > 0 ? 0 : 2;
}
