#define _POSIX_C_SOURCE 200809L

#include "rb.h"
#include "rb_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

/*
 * Backpressure in a single process, without any blocking primitive.
 *
 * The producer runs in its own thread. It does NOT spin or block when
 * the ring is full. Instead, the ring's latched callbacks set and
 * clear a `stop_reading` flag, and the producer checks that flag
 * between items.
 *
 * This is the cooperative pattern: the ring reports state, the
 * producer decides when to slow down.
 *
 * Run with -m wait (default) or -m drop to see both policies.
 */

#define CAPACITY    32u
#define SLOTS       32u
#define SLOT_SIZE   1024u
#define ITEMS       2000u
#define BURST       16u

typedef struct {
    rb_t *rb;
    atomic_bool stop_reading;   /* set by on_full, cleared by on_low_d */
    atomic_uint_fast64_t produced;
    atomic_uint_fast64_t dropped;
    atomic_uint_fast64_t consumed;
    int drop_mode;
} ctx_t;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---------------- callbacks ---------------- */

static void on_full(rb_t *rb, void *user) {
    (void)rb;
    ctx_t *c = user;
    atomic_store(&c->stop_reading, true);
}

static void on_low_d(rb_t *rb, void *user) {
    (void)rb;
    ctx_t *c = user;
    atomic_store(&c->stop_reading, false);
}

/* ---------------- producer ---------------- */

static void *producer_fn(void *arg) {
    ctx_t *c = arg;

    for (uint32_t i = 0; i < ITEMS; ) {
        /* If the ring is signalling full, back off. This is the
           cooperative backpressure path. The consumer will fire
           on_low_d once it has drained enough, clearing the flag. */
        if (atomic_load(&c->stop_reading) && !c->drop_mode) {
            sleep_ms(1);
            continue;
        }

        uint32_t idx, cap;
        void *w;
        rb_err_t e = rb_acquire(c->rb, 32, &idx, &w, &cap);

        if (e == RB_ERR_FULL) {
            if (c->drop_mode) {
                atomic_fetch_add(&c->dropped, 1);
                i++;
                continue;
            }
            atomic_store(&c->stop_reading, true);
            sleep_ms(1);
            continue;
        }
        if (e != RB_OK) break;

        int len = snprintf(w, 32, "item %u", i);
        rb_publish(c->rb, idx, (uint32_t)len);
        atomic_fetch_add(&c->produced, 1);
        i++;
    }
    return NULL;
}

/* ---------------- consumer ---------------- */

static bool on_item(const void *obj, uint32_t len, bool trunc, void *user) {
    ctx_t *c = user;
    (void)obj; (void)len;
    if (trunc) return true;
    atomic_fetch_add(&c->consumed, 1);
    /* Simulate slow processing. */
    struct timespec ts = { 0, 200000L };   /* 200 us */
    nanosleep(&ts, NULL);
    return true;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    int drop_mode = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            ++i;
            if (!strcmp(argv[i], "drop")) drop_mode = 1;
        }
    }

    void *rb_mem = malloc(rb_size(CAPACITY));
    void *scratch = malloc(SLOTS * SLOT_SIZE);
    if (!rb_mem || !scratch) { fprintf(stderr, "oom\n"); return 1; }

    rb_t *rb = (rb_t *)rb_mem;

    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rb = rb;
    ctx.drop_mode = drop_mode;
    atomic_init(&ctx.stop_reading, false);
    atomic_init(&ctx.produced, 0);
    atomic_init(&ctx.dropped, 0);
    atomic_init(&ctx.consumed, 0);

    rb_callbacks_t cb = {
        .on_full  = on_full,
        .on_low_d = on_low_d,
        .user     = &ctx,
    };

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = CAPACITY;
    cfg.slots     = SLOTS;
    cfg.slot_size = SLOT_SIZE;
    cfg.low_d     = 30;    /* clear stop_reading when count drops below 30% */
    cfg.cb        = cb;

    if (rb_init(rb, &cfg, scratch, SLOTS * SLOT_SIZE) != RB_OK) {
        fprintf(stderr, "rb_init failed\n");
        return 1;
    }

    printf("backpressure demo, mode=%s, low_d=30%%\n",
           drop_mode ? "drop" : "wait");

    rb_thread_t tp;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    rb_thread_spawn(rb, &tp, true,  producer_fn, &ctx);
//    rb_thread_spawn(rb, &tc, false,
//                    (void *(*)(void *))0, NULL);   /* placeholder */
    rb_thread_join(&tp);

    /* consumer runs inline: drain the ring until producer is done */
    for (;;) {
        uint32_t n = rb_drain(rb, on_item, &ctx);
        if (n == 0 && atomic_load(&ctx.produced) + atomic_load(&ctx.dropped) >= ITEMS)
            break;
        if (n == 0) sleep_ms(1);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("produced=%llu dropped=%llu consumed=%llu in %.2fs\n",
           (unsigned long long)atomic_load(&ctx.produced),
           (unsigned long long)atomic_load(&ctx.dropped),
           (unsigned long long)atomic_load(&ctx.consumed),
           secs);

#if RB_ENABLE_STATS
    const rb_stats_t *st = rb_stats(rb);
    printf("stats: high_water=%u full_hits=%u low_d_hits=%u\n",
           st->high_water, st->full_hits, st->low_d_hits);
    printf("latched: full_hits should be ~ number of overload episodes, "
           "not %u\n", ITEMS);
#endif

    rb_deinit(rb);
    free(scratch);
    free(rb_mem);
    return 0;
}
