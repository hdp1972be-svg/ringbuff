/* SPDX-License-Identifier: MIT */
/*
 * CPU side of the Zynq dual-ring offload example.
 *
 * Simulates “NIC / WebSocket JSON arrived” by publishing a few JSON
 * frames into ring A.  Then drains hashed results from ring B.
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
#include <unistd.h>

struct zo_shared *g_zo;

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

/* Fake “WebSocket JSON from the NIC”. */
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
    /* Optional: stash sequence in the first 4 bytes if the FPGA needs it.
     * Here the FPGA just hashes the whole payload. */
    (void)seq;

    if (rb_publish(ring, idx, len) != RB_OK) {
        fprintf(stderr, "rb_publish failed\n");
        return -1;
    }
    /* RB_HW_FLUSH_SLOT + RB_HW_NOTIFY_DEVICE already ran inside publish. */
    return 0;
}

static int drain_results(rb_t *ring, int expected)
{
    int got = 0;
    while (got < expected) {
        uint32_t idx = 0, len = 0;
        const void *obj = NULL;
        bool trunc = false;

        if (rb_consume(ring, &idx, &obj, &len, &trunc) != RB_OK) {
            /* Poll the doorbell the FPGA sets (stand-in for IRQ). */
            if (g_zo->bell.fpga_to_cpu) {
                g_zo->bell.fpga_to_cpu = 0;
                continue;
            }
            usleep(200);
            continue;
        }

        if (len < sizeof(struct zo_result)) {
            fprintf(stderr, "short result len=%u\n", len);
            rb_release(ring, idx);
            continue;
        }

        const struct zo_result *r = (const struct zo_result *)obj;
        printf("CPU  result: seq=%u hash=0x%08x in_len=%u tag=%.4s%s\n",
               r->seq, r->hash, r->in_len, r->tag,
               trunc ? " (trunc)" : "");
        rb_release(ring, idx);
        got++;
    }
    return got;
}

int main(void)
{
    printf("zynq_offload cpu_host — creating shared region\n");
    g_zo = map_shared(1);
    if (!g_zo)
        return 1;

    if (init_rings(g_zo) != 0) {
        fprintf(stderr, "rb_init failed\n");
        return 1;
    }

    const int n = (int)(sizeof sample_json / sizeof sample_json[0]);
    printf("publishing %d WS-JSON frames into ingress ring A\n", n);

    for (int i = 0; i < n; i++) {
        uint32_t seq = ++g_zo->bell.seq_in;
        if (publish_json(zo_ring_a(g_zo), sample_json[i], seq) != 0)
            return 1;
        printf("CPU  published seq=%u len=%zu\n",
               seq, strlen(sample_json[i]));
    }

    printf("waiting for FPGA results on egress ring B …\n");
    int got = drain_results(zo_ring_b(g_zo), n);
    printf("done: got %d / %d results\n", got, n);

    /* Leave shm in place so the stub can exit cleanly; user may rm it. */
    return got == n ? 0 : 2;
}
