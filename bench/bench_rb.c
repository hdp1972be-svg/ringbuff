/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

#define _POSIX_C_SOURCE 200809L

#include "rb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *xaligned(size_t align, size_t sz) {
    void *p = NULL;
    if (posix_memalign(&p, align, sz) || !p) {
        fprintf(stderr, "alloc failed\n"); exit(2);
    }
    return p;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void bench_one(uint32_t slot_size, uint32_t cap, uint32_t iters) {
    size_t rb_bytes = rb_size(cap);
    void *rb_mem  = xaligned(RB_CACHE_LINE, rb_bytes);
    size_t scratch_sz = (size_t)cap * (size_t)slot_size * 2u; /* generous */
    void *scratch = xaligned(RB_CACHE_LINE, scratch_sz);

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = cap;
    cfg.slots     = cap;
    cfg.slot_size = slot_size;

    rb_t *rb = (rb_t *)rb_mem;
    if (rb_init(rb, &cfg, scratch, scratch_sz) != RB_OK) {
        fprintf(stderr, "rb_init failed\n"); exit(2);
    }

    uint32_t payload = slot_size - RB_SLOT_HDR_SIZE;

    /* warm-up */
    for (uint32_t i = 0; i < 20000u; ++i) {
        uint32_t idx, c; void *w;
        rb_acquire(rb, payload, &idx, &w, &c);
        rb_publish(rb, idx, payload);
        uint32_t ci, cl; const void *o; bool t;
        rb_consume(rb, &ci, &o, &cl, &t);
        rb_release(rb, ci);
    }

    double t0 = now_sec();
    for (uint32_t i = 0; i < iters; ++i) {
        uint32_t idx, c; void *w;
        rb_acquire(rb, payload, &idx, &w, &c);
        rb_publish(rb, idx, payload);
        uint32_t ci, cl; const void *o; bool t;
        rb_consume(rb, &ci, &o, &cl, &t);
        rb_release(rb, ci);
    }
    double t1 = now_sec();

    double cycles_per_s = (double)iters / (t1 - t0);
    double ns_per_cycle = 1e9 / cycles_per_s;

    printf("  slot=%-6u cap=%-5u  %8.2f M cycles/s  (%7.2f ns / cycle)\n",
           slot_size, cap, cycles_per_s / 1e6, ns_per_cycle);

    rb_deinit(rb);
    free(scratch);
    free(rb_mem);
}

int main(void) {
    printf("rb single-threaded full cycle benchmark\n");
    printf("(1 cycle = acquire + publish + consume + release)\n");
    printf("-------------------------------------------------\n");

    const uint32_t iters = 2000000u;
    const uint32_t sizes[] = { 64u, 256u, 2048u, 8192u };
    const uint32_t caps[]  = { 64u, 1024u };

    for (size_t c = 0; c < sizeof(caps)/sizeof(caps[0]); ++c) {
        printf("\ncapacity=%u:\n", caps[c]);
        for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
            bench_one(sizes[s], caps[c], iters);
        }
    }
    return 0;
}
