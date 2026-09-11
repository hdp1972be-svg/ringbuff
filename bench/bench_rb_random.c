/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

#define _POSIX_C_SOURCE 200809L

#include "rb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------------- PRNG ---------------- */

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint64_t rng_next(void) {
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

static uint32_t rng_below(uint32_t bound) {
    if (bound == 0) return 0;
    return (uint32_t)(rng_next() % bound);
}

static void seed_from_urandom(void) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        uint64_t s = 0;
        ssize_t r = read(fd, &s, sizeof s);
        close(fd);
        if (r == (ssize_t)sizeof s && s != 0) g_rng = s;
    }
}

/* ---------------- helpers ---------------- */

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

/* Fill a buffer with PRNG bytes. Called once before benchmarking. */
static void fill_random(uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)rng_next();
}

/* ---------------- benchmark: fixed size, random content ------------- */

static void bench_fixed(uint32_t slot_size, uint32_t cap,
                        uint32_t payload, uint32_t iters)
{
    size_t rb_bytes   = rb_size(cap);
    void  *rb_mem     = xaligned(RB_CACHE_LINE, rb_bytes);
    size_t scratch_sz = (size_t)cap * (size_t)slot_size;
    void  *scratch    = xaligned(RB_CACHE_LINE, scratch_sz);

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = cap;
    cfg.slots     = cap;
    cfg.slot_size = slot_size;

    rb_t *rb = (rb_t *)rb_mem;
    if (rb_init(rb, &cfg, scratch, scratch_sz) != RB_OK) {
        fprintf(stderr, "rb_init failed\n"); exit(2);
    }

    /* Source pool of random bytes; indexed by a rolling offset. */
    const size_t pool_size = 1u << 20;   /* 1 MB */
    uint8_t *pool = xaligned(RB_CACHE_LINE, pool_size);
    fill_random(pool, pool_size);

    /* Warm-up */
    for (uint32_t i = 0; i < 10000u; ++i) {
        uint32_t idx, c; void *w;
        rb_acquire(rb, payload, &idx, &w, &c);
        memcpy(w, pool, payload);
        rb_publish(rb, idx, payload);
        uint32_t ci, cl; const void *o; bool t;
        rb_consume(rb, &ci, &o, &cl, &t);
        rb_release(rb, ci);
    }

    uint64_t checksum = 0;
    size_t   pool_off = 0;
    double t0 = now_sec();

    for (uint32_t i = 0; i < iters; ++i) {
        uint32_t idx, c; void *w;
        if (rb_acquire(rb, payload, &idx, &w, &c) != RB_OK) continue;

        size_t src = pool_off;
        size_t end = src + payload;
        if (end > pool_size) {
            size_t first = pool_size - src;
            memcpy(w, pool + src, first);
            memcpy((uint8_t *)w + first, pool, payload - first);
        } else {
            memcpy(w, pool + src, payload);
        }
        pool_off = (end >= pool_size) ? (end - pool_size) : end;

        rb_publish(rb, idx, payload);

        uint32_t ci, cl; const void *o; bool t;
        rb_consume(rb, &ci, &o, &cl, &t);
        /* touch a few bytes so the copy isn't dead-code-eliminated */
        const uint8_t *r = (const uint8_t *)o;
        checksum ^= ((uint64_t)r[0] << 56)
                  ^ ((uint64_t)r[cl >> 1] << 24)
                  ^ ((uint64_t)r[cl - 1]);
        rb_release(rb, ci);
    }

    double t1 = now_sec();
    double secs = t1 - t0;
    double items_per_s = (double)iters / secs;
    double bytes_per_s = items_per_s * (double)payload;

    printf("  slot=%-5u cap=%-5u payload=%-5u  "
           "%8.2f M items/s  %8.2f MB/s   (chk=%08llx)\n",
           slot_size, cap, payload,
           items_per_s / 1e6, bytes_per_s / (1024.0 * 1024.0),
           (unsigned long long)(checksum & 0xffffffffu));

    rb_deinit(rb);
    free(scratch);
    free(rb_mem);
    free(pool);
}

/* ---------------- benchmark: variable size, random content ------------- */

typedef struct {
    uint32_t min;
    uint32_t max;
} len_dist_t;

static void bench_variable(uint32_t slot_size, uint32_t cap,
                           len_dist_t dist, uint32_t iters)
{
    size_t rb_bytes   = rb_size(cap);
    void  *rb_mem     = xaligned(RB_CACHE_LINE, rb_bytes);
    size_t scratch_sz = (size_t)cap * (size_t)slot_size;
    void  *scratch    = xaligned(RB_CACHE_LINE, scratch_sz);

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = cap;
    cfg.slots     = cap;
    cfg.slot_size = slot_size;

    rb_t *rb = (rb_t *)rb_mem;
    if (rb_init(rb, &cfg, scratch, scratch_sz) != RB_OK) {
        fprintf(stderr, "rb_init failed\n"); exit(2);
    }

    const size_t pool_size = 1u << 20;
    uint8_t *pool = xaligned(RB_CACHE_LINE, pool_size);
    fill_random(pool, pool_size);

    /* Pre-generate lengths so the hot loop measures only the ring. */
    uint32_t *lengths = malloc((size_t)iters * sizeof *lengths);
    if (!lengths) { fprintf(stderr, "oom\n"); exit(2); }
    uint32_t span = dist.max - dist.min + 1u;
    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < iters; ++i) {
        lengths[i] = dist.min + rng_below(span);
        total_bytes += lengths[i];
    }

    uint64_t checksum = 0;
    size_t   pool_off = 0;
    double t0 = now_sec();

    for (uint32_t i = 0; i < iters; ++i) {
        uint32_t payload = lengths[i];
        uint32_t idx, c; void *w;
        if (rb_acquire(rb, payload, &idx, &w, &c) != RB_OK) continue;
        if (payload > c) payload = c;   /* truncate for the write */

        size_t src = pool_off;
        size_t end = src + payload;
        if (end > pool_size) {
            size_t first = pool_size - src;
            memcpy(w, pool + src, first);
            memcpy((uint8_t *)w + first, pool, payload - first);
        } else {
            memcpy(w, pool + src, payload);
        }
        pool_off = (end >= pool_size) ? (end - pool_size) : end;

        rb_publish(rb, idx, payload);

        uint32_t ci, cl; const void *o; bool t;
        rb_consume(rb, &ci, &o, &cl, &t);
        const uint8_t *r = (const uint8_t *)o;
        if (cl > 0) {
            checksum ^= ((uint64_t)r[0] << 32)
                      ^ ((uint64_t)r[cl - 1]);
        }
        rb_release(rb, ci);
    }

    double t1 = now_sec();
    double secs = t1 - t0;
    double items_per_s = (double)iters / secs;
    double bytes_per_s = (double)total_bytes / secs;
    double avg = (double)total_bytes / (double)iters;

    printf("  slot=%-5u cap=%-5u len=[%u..%u] avg=%.0f  "
           "%8.2f M items/s  %8.2f MB/s   (chk=%08llx)\n",
           slot_size, cap, dist.min, dist.max, avg,
           items_per_s / 1e6, bytes_per_s / (1024.0 * 1024.0),
           (unsigned long long)(checksum & 0xffffffffu));

    free(lengths);
    rb_deinit(rb);
    free(scratch);
    free(rb_mem);
    free(pool);
}

/* ---------------- main ---------------- */

int main(void) {
    seed_from_urandom();

    printf("rb random-data benchmark\n");
    printf("(payloads written from a 1 MB PRNG pool, per-item checksum)\n");
    printf("seed = 0x%016llx\n",
           (unsigned long long)g_rng);
    printf("-----------------------------------------------------------\n");

    const uint32_t iters = 1000000u;

    printf("\nfixed-size payloads (slot=2048):\n");
    bench_fixed(2048, 64, 16,    iters);
    bench_fixed(2048, 64, 256,   iters);
    bench_fixed(2048, 64, 1024,  iters);
    bench_fixed(2048, 64, 2044,  iters);
    bench_fixed(8192, 1024, 4096, iters);   /* 8 MB working set — expect the cliff */

    printf("\nvariable-size payloads (slot=2048, WS-like):\n");
    bench_variable(2048, 64, (len_dist_t){  16,  128}, iters);
    bench_variable(2048, 64, (len_dist_t){ 128, 1024}, iters);
    bench_variable(2048, 64, (len_dist_t){ 256, 2044}, iters);

    printf("\nvariable-size payloads (slot=8192, larger frames):\n");
    bench_variable(8192, 64, (len_dist_t){ 512, 4096}, iters);
    bench_variable(8192, 64, (len_dist_t){2048, 8192}, iters);

    return 0;
}
