/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

#define _POSIX_C_SOURCE 200809L

#include "rb.h"
#include "rb_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

/* ---------------- harness ---------------- */

static int g_failures = 0;

#define CHECK(cond, msg) do {                                             \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
        g_failures++;                                                     \
    }                                                                     \
} while (0)

static void *aligned_alloc_or_die(size_t align, size_t size) {
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0 || !p) {
        fprintf(stderr, "posix_memalign failed\n");
        exit(2);
    }
    return p;
}

/* ---------------- shared test state ---------------- */

#define SLOT_SIZE   128u
#define NUM_SLOTS   64u
#define RING_CAP    64u
#define ITEMS       2000000u   /* total items to push through */

typedef struct {
    rb_t  *rb;
    atomic_uint_fast64_t produced;
    atomic_uint_fast64_t consumed;
    atomic_bool stop;
    uint32_t payload_size;   /* bytes per item */
    /* verification state (consumer only) */
    uint64_t next_expected;
    uint64_t errors;
} test_ctx_t;

/* ---------------- producer ---------------- */

static void *producer_fn(void *arg) {
    test_ctx_t *ctx = (test_ctx_t *)arg;

    for (uint64_t seq = 0; seq < ITEMS; ) {
        uint32_t idx, cap;
        void *w;
        rb_err_t e = rb_acquire(ctx->rb, ctx->payload_size,
                                &idx, &w, &cap);
        if (e == RB_ERR_FULL) continue;    /* spin until space */
        if (e != RB_OK) {
            fprintf(stderr, "producer: acquire err %d\n", (int)e);
            atomic_store(&ctx->stop, true);
            return NULL;
        }

        /* Write a deterministic pattern derived from seq, plus the
           sequence number itself in the first 8 bytes. */
        uint8_t *p = (uint8_t *)w;
        uint32_t n = ctx->payload_size;
        if (n < 8) n = 8;                  /* ensure room for seq */
        if (n > cap) n = cap;

        memcpy(p, &seq, sizeof seq);
        for (uint32_t i = 8; i < n; ++i) {
            p[i] = (uint8_t)(seq ^ (seq >> 8) ^ i);
        }

        e = rb_publish(ctx->rb, idx, n);
        if (e != RB_OK) {
            fprintf(stderr, "producer: publish err %d\n", (int)e);
            atomic_store(&ctx->stop, true);
            return NULL;
        }

        atomic_fetch_add(&ctx->produced, 1u);
        seq++;
    }
    return NULL;
}

/* ---------------- consumer ---------------- */

static void *consumer_fn(void *arg) {
    test_ctx_t *ctx = (test_ctx_t *)arg;

    while (ctx->next_expected < ITEMS) {
        uint32_t idx, len;
        const void *obj;
        bool trunc;
        rb_err_t e = rb_consume(ctx->rb, &idx, &obj, &len, &trunc);
        if (e == RB_ERR_EMPTY) {
            if (atomic_load(&ctx->stop)) break;
            continue;
        }
        if (e != RB_OK) {
            fprintf(stderr, "consumer: consume err %d\n", (int)e);
            ctx->errors++;
            break;
        }

        /* Verify sequence: first 8 bytes hold the expected seq. */
        uint64_t got = 0;
        memcpy(&got, obj, sizeof got);
        if (got != ctx->next_expected) {
            fprintf(stderr,
                    "consumer: seq mismatch, expected %llu got %llu\n",
                    (unsigned long long)ctx->next_expected,
                    (unsigned long long)got);
            ctx->errors++;
            /* keep going to drain the ring, don't hang the producer */
        } else {
            /* Verify payload pattern matches what the producer wrote. */
            const uint8_t *p = (const uint8_t *)obj;
            for (uint32_t i = 8; i < len; ++i) {
                uint8_t expect = (uint8_t)(got ^ (got >> 8) ^ i);
                if (p[i] != expect) {
                    fprintf(stderr,
                            "consumer: payload corruption at seq %llu off %u\n",
                            (unsigned long long)got, i);
                    ctx->errors++;
                    break;
                }
            }
        }

        rb_release(ctx->rb, idx);
        atomic_fetch_add(&ctx->consumed, 1u);
        ctx->next_expected++;
    }
    return NULL;
}

/* ---------------- callback counters ---------------- */

static atomic_uint cb_added, cb_full, cb_low_d, cb_low_e;

static void on_added(rb_t *rb, uint32_t idx, uint32_t len,
                     bool trunc, void *user) {
    (void)rb; (void)idx; (void)len; (void)trunc; (void)user;
    atomic_fetch_add(&cb_added, 1u);
}
static void on_full(rb_t *rb, void *user) {
    (void)rb; (void)user; atomic_fetch_add(&cb_full, 1u);
}
static void on_low_d(rb_t *rb, void *user) {
    (void)rb; (void)user; atomic_fetch_add(&cb_low_d, 1u);
}
static void on_low_e(rb_t *rb, void *user) {
    (void)rb; (void)user; atomic_fetch_add(&cb_low_e, 1u);
}

/* ---------------- main ---------------- */

int main(void) {
    /* Control block + scratchpad, aligned to cache line. */
    size_t rb_bytes = rb_size(RING_CAP);
    void *rb_mem  = aligned_alloc_or_die(RB_CACHE_LINE, rb_bytes);
    void *scratch = aligned_alloc_or_die(RB_CACHE_LINE,
                                         NUM_SLOTS * SLOT_SIZE);

    rb_callbacks_t cb;
    memset(&cb, 0, sizeof cb);
    cb.on_slot_added = on_added;
    cb.on_full       = on_full;
    cb.on_low_d      = on_low_d;
    cb.on_low_e      = on_low_e;

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = RING_CAP;
    cfg.slots     = NUM_SLOTS;
    cfg.slot_size = SLOT_SIZE;
    cfg.low_d     = 25;
    cfg.low_e     = 10;
    cfg.cb        = cb;

    /* Small stacks on purpose, to also exercise the stack setters. */
    rb_config_set_producer_stack(&cfg, 128u * 1024u);
    rb_config_set_consumer_stack(&cfg, 128u * 1024u);

    rb_t *rb = (rb_t *)rb_mem;
    CHECK(rb_init(rb, &cfg, scratch, NUM_SLOTS * SLOT_SIZE) == RB_OK,
          "rb_init");

    test_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rb = rb;
    ctx.payload_size = SLOT_SIZE - RB_SLOT_HDR_SIZE;  /* full payload */
    atomic_init(&ctx.produced, 0);
    atomic_init(&ctx.consumed, 0);
    atomic_init(&ctx.stop, false);
    ctx.next_expected = 0;
    ctx.errors = 0;

    atomic_init(&cb_added, 0);
    atomic_init(&cb_full, 0);
    atomic_init(&cb_low_d, 0);
    atomic_init(&cb_low_e, 0);

    rb_thread_t tp, tc;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    CHECK(rb_thread_spawn(rb, &tp, true,  producer_fn, &ctx) == RB_OK,
          "spawn producer");
    CHECK(rb_thread_spawn(rb, &tc, false, consumer_fn, &ctx) == RB_OK,
          "spawn consumer");

    rb_thread_join(&tp);
    rb_thread_join(&tc);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    uint64_t prod = atomic_load(&ctx.produced);
    uint64_t cons = atomic_load(&ctx.consumed);

    CHECK(prod == ITEMS, "producer pushed all items");
    CHECK(cons == ITEMS, "consumer pulled all items");
    CHECK(ctx.errors == 0, "no sequencing/payload errors");
    CHECK(ctx.next_expected == ITEMS, "consumer saw every seq in order");

    /* Stats sanity: published should equal produced. */
#if RB_ENABLE_STATS
    const rb_stats_t *st = rb_stats(rb);
    CHECK(st->published == prod, "stats.published == produced");
    CHECK(st->consumed  == cons, "stats.consumed  == consumed");
    CHECK(atomic_load(&cb_added) == prod, "on_slot_added fired per publish");
#endif

    printf("threaded spsc: %llu items in %.3f s (%.1f M items/s)\n",
           (unsigned long long)ITEMS, secs, (double)ITEMS / secs / 1e6);
    printf("callbacks: added=%u full=%u low_d=%u low_e=%u\n",
           atomic_load(&cb_added), atomic_load(&cb_full),
           atomic_load(&cb_low_d), atomic_load(&cb_low_e));

    rb_deinit(rb);
    free(scratch);
    free(rb_mem);

    printf("threaded test: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
