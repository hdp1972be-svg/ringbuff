/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/*
 * CPU side of the Zynq dual-ring offload example.
 *
 * Two modes:
 *   1) Demo (default, small -n): publish a few sample WS-JSON frames.
 *   2) Stress: publish many fixed-size payloads as fast as possible,
 *      count drops (or wait), drain results, and at the end print a
 *      theoretical msg/sec that would keep the ring free of drops
 *      under the current parameters.
 *
 * On a real Antminer S9 replace the shm_open path with mmap of the
 * reserved DDR/BRAM window and feed real socket data into the publish
 * loop.
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

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
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

    rb_err_t e = rb_init(zo_ring_a(s), &cfg, s->scratch_a, sizeof s->scratch_a);
    if (e != RB_OK) {
        fprintf(stderr, "rb_init(ring_a) = %d\n", (int)e);
        return -1;
    }
    e = rb_init(zo_ring_b(s), &cfg, s->scratch_b, sizeof s->scratch_b);
    if (e != RB_OK) {
        fprintf(stderr, "rb_init(ring_b) = %d\n", (int)e);
        return -1;
    }

    s->bell.cpu_to_fpga = 0;
    s->bell.fpga_to_cpu = 0;
    s->bell.seq_in = 0;
    s->bell.seq_out = 0;
    return 0;
}

/* Fake “WebSocket JSON from the NIC” (demo mode only). */
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

    for (;;) {
        rb_err_t e = rb_acquire(ring, want, &idx, &w, &cap);
        if (e == RB_OK)
            break;
        if (e == RB_ERR_FULL) {
            if (full_hits)
                (*full_hits)++;
            if (drop_mode)
                return 1; /* signal drop */
            usleep(50);
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

static int drain_results(rb_t *ring, int expected, int quiet)
{
    int got = 0;
    int idle = 0;
    while (got < expected && idle < 500) { /* ~1 s max idle */
        uint32_t idx = 0, len = 0;
        const void *obj = NULL;
        bool trunc = false;

        if (rb_consume(ring, &idx, &obj, &len, &trunc) != RB_OK) {
            if (g_zo->bell.fpga_to_cpu) {
                g_zo->bell.fpga_to_cpu = 0;
                idle = 0;
                continue;
            }
            usleep(2000);
            idle++;
            continue;
        }

        idle = 0;
        if (len < sizeof(struct zo_result)) {
            fprintf(stderr, "short result len=%u\n", len);
            rb_release(ring, idx);
            continue;
        }

        const struct zo_result *r = (const struct zo_result *)obj;
        if (!quiet) {
            printf("CPU  result: seq=%u hash=0x%08x in_len=%u tag=%.4s%s\n",
                   r->seq, r->hash, r->in_len, r->tag,
                   trunc ? " (trunc)" : "");
        }
        rb_release(ring, idx);
        got++;
    }
    return got;
}

static void usage(const char *prog)
{
    printf(
        "usage: %s [options]\n"
        "  -n N     total messages to publish (default 5 = demo JSON,\n"
        "           use e.g. 10000 for stress)\n"
        "  -s BYTES payload size for stress mode (default 64, max %u)\n"
        "  -m MODE  wait | drop  (default drop for stress, wait for demo)\n"
        "  -v       verbose (print every result in stress mode)\n"
        "  -h       this help\n"
        "\n"
        "Stress mode (-n large) measures drops and prints a theoretical\n"
        "msg/sec that would avoid drops under the current ring parameters.\n",
        prog, (unsigned)(ZO_SLOT_SIZE - 16u));
}

int main(int argc, char **argv)
{
    uint64_t total     = 5;          /* default: demo with sample_json */
    uint32_t msg_size  = 64u;
    int      drop_mode = -1;         /* -1 = auto */
    int      verbose   = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)
            total = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            msg_size = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            ++i;
            if (!strcmp(argv[i], "drop")) drop_mode = 1;
            else if (!strcmp(argv[i], "wait")) drop_mode = 0;
            else { fprintf(stderr, "unknown mode: %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (msg_size < 4u || msg_size > ZO_SLOT_SIZE - 16u) {
        fprintf(stderr, "payload size %u out of range [4, %u]\n",
                msg_size, (unsigned)(ZO_SLOT_SIZE - 16u));
        return 1;
    }

    int demo = (total <= 5);
    if (drop_mode < 0)
        drop_mode = demo ? 0 : 1;   /* wait for tiny demo, drop for stress */

    printf("zynq_offload cpu_host — creating shared region\n");
    printf("  params: capacity=%u slots=%u slot_size=%u payload=%u mode=%s\n",
           ZO_CAPACITY, ZO_SLOTS, ZO_SLOT_SIZE, msg_size,
           drop_mode ? "drop" : "wait");

    g_zo = map_shared(1);
    if (!g_zo)
        return 1;

    if (init_rings(g_zo) != 0) {
        fprintf(stderr, "rb_init failed (check capacity/slots/slot_size vs scratch)\n");
        fprintf(stderr, "  capacity=%u slots=%u slot_size=%u scratch_a=%zu\n",
                ZO_CAPACITY, ZO_SLOTS, ZO_SLOT_SIZE, sizeof g_zo->scratch_a);
        return 1;
    }

    /* Prepare a fixed payload for stress mode. */
    char *payload = NULL;
    if (!demo) {
        payload = (char *)malloc(msg_size);
        if (!payload) {
            perror("malloc");
            return 1;
        }
        memset(payload, 0xA5, msg_size);
        /* Put a simple header so hash changes per seq if we want. */
        memcpy(payload, "ZYNQ", 4);
    }

    printf("publishing %llu messages into ingress ring A\n",
           (unsigned long long)total);

    double t0 = now_sec();
    uint64_t published = 0;
    uint64_t dropped   = 0;
    uint64_t full_hits = 0;

    for (uint64_t i = 0; i < total; i++) {
        uint32_t seq = ++g_zo->bell.seq_in;
        int rc;

        if (demo) {
            const char *json = sample_json[i % 5];
            uint32_t want = (uint32_t)strlen(json) + 1u;
            rc = publish_payload(zo_ring_a(g_zo), json, want, drop_mode, &full_hits);
        } else {
            /* Overwrite first 8 bytes with seq for uniqueness. */
            memcpy(payload + 4, &seq, sizeof seq);
            rc = publish_payload(zo_ring_a(g_zo), payload, msg_size,
                                 drop_mode, &full_hits);
        }

        if (rc < 0)
            return 1;
        if (rc == 1) {
            dropped++;
            continue;
        }
        published++;
        if (demo || verbose)
            printf("CPU  published seq=%u\n", seq);
    }

    double t_pub = now_sec() - t0;
    printf("publish done: published=%llu dropped=%llu full_hits=%llu "
           "t=%.3fs (%.0f msg/s attempted)\n",
           (unsigned long long)published,
           (unsigned long long)dropped,
           (unsigned long long)full_hits,
           t_pub,
           t_pub > 0 ? (double)total / t_pub : 0.0);

    printf("waiting for FPGA results on egress ring B …\n");
    double t_drain0 = now_sec();
    int got = drain_results(zo_ring_b(g_zo), (int)published, demo || verbose ? 0 : 1);
    double t_drain = now_sec() - t_drain0;
    double t_total = now_sec() - t0;

    printf("done: got %d / %llu results  drain=%.3fs total=%.3fs\n",
           got, (unsigned long long)published, t_drain, t_total);

    /* Theoretical rate that would produce zero drops under these params. */
    double consumer_rate = 0.0;
    if (got > 0 && t_total > 0.0)
        consumer_rate = (double)got / t_total;

    printf("\n=== Theoretical no-drop rate ===\n");
    printf("Ring parameters kept constant:\n");
    printf("  capacity=%u  slots=%u  slot_size=%u  payload=%u  mode=%s\n",
           ZO_CAPACITY, ZO_SLOTS, ZO_SLOT_SIZE, msg_size,
           drop_mode ? "drop" : "wait");
    printf("Observed consumer throughput (results / wall time): %.0f msg/s\n",
           consumer_rate);
    if (dropped > 0) {
        printf("Drops occurred because the producer outran the FPGA stub.\n");
        printf("If the producer stays at or below ~%.0f msg/s with the same\n"
               "parameters above, the ingress ring will not fill and no drops\n"
               "will occur (the FPGA can keep up).\n",
               consumer_rate);
    } else {
        printf("No drops observed. The producer rate was already safe.\n");
        printf("A higher sustainable rate is still bounded by the consumer\n"
               "(~%.0f msg/s under these parameters and host load).\n",
               consumer_rate);
    }
    printf("================================\n");

    free(payload);
    /* Leave shm in place so the stub can exit cleanly; user may rm it. */
    return (got == (int)published) ? 0 : 2;
}
