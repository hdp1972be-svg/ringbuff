/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

#define _POSIX_C_SOURCE 200809L

#include "rb.h"
#include "rb_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#include <time.h>

/* Scenario: fast producer, slow consumer. The ring is deliberately
   undersized. The producer must see RB_ERR_FULL many times and yield.
   We verify:
     - no loss, no reordering, no payload corruption
     - the ring actually saturates (high_water == limit)
     - the producer is genuinely throttled (full_attempts > 0) */

#define SLOT_SIZE     2048u
#define NUM_SLOTS     64u
#define RING_CAP      64u
#define TOTAL_ITEMS   200000u
#define ITEM_BYTES    8u

typedef struct {
    rb_t *rb;
    atomic_uint_fast64_t produced;
    atomic_uint_fast64_t consumed;
    atomic_uint_fast64_t full_attempts;
    atomic_bool producer_done;
    uint64_t next_expected;
    uint64_t errors;
    uint64_t consumer_delay_ns;
} ctx_t;

static void *producer_fn(void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (uint64_t seq = 0; seq < TOTAL_ITEMS; ) {
        uint32_t idx, cap;
        void *w;
        rb_err_t e = rb_acquire(c->rb, ITEM_BYTES, &idx, &w, &cap);
        if (e == RB_ERR_FULL) {
            atomic_fetch_add(&c->full_attempts, 1u);
            sched_yield();
            continue;
        }
        if (e != RB_OK) {
            fprintf(stderr, "producer: acquire err %d\n", (int)e);
            c->errors++;
            break;
        }
        memcpy(w, &seq, sizeof seq);
        rb_publish(c->rb, idx, ITEM_BYTES);
        atomic_fetch_add(&c->produced, 1u);
        seq++;
    }
    atomic_store(&c->producer_done, true);
    return NULL;
}

static void *consumer_fn(void *arg) {
    ctx_t *c = (ctx_t *)arg;

    while (c->next_expected < TOTAL_ITEMS) {
        uint32_t idx, len;
        const void *obj;
        bool tr;
        rb_err_t e = rb_consume(c->rb, &idx, &obj, &len, &tr);
        if (e == RB_ERR_EMPTY) {
            if (atomic_load(&c->producer_done) && rb_is_empty(c->rb)) break;
            sched_yield();
            continue;
        }
        if (e != RB_OK) {
            fprintf(stderr, "consumer: consume err %d\n", (int)e);
            c->errors++;
            break;
        }
        uint64_t got = 0;
        memcpy(&got, obj, sizeof got);
        if (got != c->next_expected) {
            fprintf(stderr, "consumer: seq mismatch at %llu got %llu\n",
                    (unsigned long long)c->next_expected,
                    (unsigned long long)got);
            c->errors++;
        }
        rb_release(c->rb, idx);
        atomic_fetch_add(&c->consumed, 1u);
        c->next_expected++;

        if (c->consumer_delay_ns) {
            struct timespec ts = {
                .tv_sec  = (time_t)(c->consumer_delay_ns / 1000000000ull),
                .tv_nsec = (long)(c->consumer_delay_ns % 1000000000ull)
            };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void *xaligned(size_t align, size_t sz) {
    void *p = NULL;
    if (posix_memalign(&p, align, sz) || !p) {
        fprintf(stderr, "alloc failed\n"); exit(2);
    }
    return p;
}

int main(void) {
    size_t rb_bytes = rb_size(RING_CAP);
    void *rb_mem  = xaligned(RB_CACHE_LINE, rb_bytes);
    void *scratch = xaligned(RB_CACHE_LINE, (size_t)NUM_SLOTS * SLOT_SIZE);

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = RING_CAP;
    cfg.slots     = NUM_SLOTS;
    cfg.slot_size = SLOT_SIZE;

    rb_t *rb = (rb_t *)rb_mem;
    if (rb_init(rb, &cfg, scratch, (size_t)NUM_SLOTS * SLOT_SIZE) != RB_OK) {
        fprintf(stderr, "rb_init failed\n"); return 2;
    }

    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rb = rb;
    /* 5 us per item -> 200k items take ~1 second on the consumer side.
       Producer runs flat-out and will block constantly. */
    ctx.consumer_delay_ns = 5000;
    atomic_init(&ctx.produced, 0);
    atomic_init(&ctx.consumed, 0);
    atomic_init(&ctx.full_attempts, 0);
    atomic_init(&ctx.producer_done, false);

    rb_thread_t tp, tc;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (rb_thread_spawn(rb, &tp, true,  producer_fn, &ctx) != RB_OK) {
        fprintf(stderr, "spawn producer failed\n"); return 2;
    }
    if (rb_thread_spawn(rb, &tc, false, consumer_fn, &ctx) != RB_OK) {
        fprintf(stderr, "spawn consumer failed\n"); return 2;
    }

    rb_thread_join(&tp);
    rb_thread_join(&tc);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    uint64_t prod = atomic_load(&ctx.produced);
    uint64_t cons = atomic_load(&ctx.consumed);
    uint64_t full = atomic_load(&ctx.full_attempts);

    printf("backpressure test: %llu items, %.2f s, %llu FULL retries\n",
           (unsigned long long)TOTAL_ITEMS, secs, (unsigned long long)full);

    int failed = 0;
    if (prod != TOTAL_ITEMS) {
        fprintf(stderr, "FAIL: produced %llu != %u\n",
                (unsigned long long)prod, TOTAL_ITEMS); failed++;
    }
    if (cons != TOTAL_ITEMS) {
        fprintf(stderr, "FAIL: consumed %llu != %u\n",
                (unsigned long long)cons, TOTAL_ITEMS); failed++;
    }
    if (ctx.errors != 0) {
        fprintf(stderr, "FAIL: %llu sequencing errors\n",
                (unsigned long long)ctx.errors); failed++;
    }
    if (full == 0) {
        fprintf(stderr, "FAIL: expected producer to be throttled\n");
        failed++;
    }

#if RB_ENABLE_STATS
    const rb_stats_t *st = rb_stats(rb);
    printf("backpressure stats: high_water=%u limit=%u full_hits=%u\n",
           st->high_water, rb_limit(rb), st->full_hits);
    if (st->high_water < rb_limit(rb)) {
        fprintf(stderr, "FAIL: ring never saturated (high_water=%u < limit=%u)\n",
                st->high_water, rb_limit(rb)); failed++;
    }
#endif

    rb_deinit(rb);
    free(scratch);
    free(rb_mem);

    if (failed) {
        printf("backpressure test: FAILED (%d)\n", failed);
        return 1;
    }
    printf("backpressure test: OK\n");
    return 0;
}
