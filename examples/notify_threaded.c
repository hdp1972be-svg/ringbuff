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
 * Notification-driven producer/consumer, both in one process, using
 * rb_wait() to block the consumer instead of spinning.
 *
 * The producer runs in a spawned thread, publishes a burst every
 * ~250 ms, then idles. The consumer blocks in rb_wait() between
 * bursts, so it uses ~0% CPU while idle.
 *
 * The correct consumer loop is:
 *
 *     for (;;) {
 *         uint32_t v = rb_notify_value(rb);   // snapshot head FIRST
 *         if (rb_drain(...) > 0) continue;
 *         if (stop) break;
 *         rb_wait(rb, v, 100);                // block until head != v
 *     }
 *
 * Snapshotting BEFORE draining is what makes the wakeup reliable.
 * If you drain first and snapshot after, a publish between the two
 * is missed and the consumer sleeps through it.
 */

#define CAPACITY    64u
#define SLOTS       64u
#define SLOT_SIZE   512u
#define ITEMS       200u
#define BURST       20u

typedef struct {
    rb_t *rb;
    atomic_uint_fast64_t produced;
    atomic_uint_fast64_t consumed;
    atomic_bool producer_done;
} ctx_t;

static void *aligned_or_die(size_t align, size_t sz) {
    void *p = NULL;
    if (posix_memalign(&p, align, sz) != 0 || !p) {
        fprintf(stderr, "oom\n");
        exit(2);
    }
    return p;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---------------- producer thread ---------------- */

static void *producer_fn(void *arg) {
    ctx_t *c = arg;

    for (uint32_t i = 0; i < ITEMS; i += BURST) {
        uint32_t n = (ITEMS - i < BURST) ? (ITEMS - i) : BURST;

        /* publish a burst back-to-back */
        for (uint32_t j = 0; j < n; ++j) {
            uint32_t idx, cap;
            void *w;
            while (rb_acquire(c->rb, 32, &idx, &w, &cap) == RB_ERR_FULL) {
                sleep_ms(1);
            }
            int len = snprintf(w, 32, "item %u", i + j);
            rb_publish(c->rb, idx, (uint32_t)len);
            atomic_fetch_add(&c->produced, 1);
        }

        /* idle for a while. consumer should sleep through this. */
        sleep_ms(250);
    }

    atomic_store(&c->producer_done, true);
    return NULL;
}

/* ---------------- consumer callback ---------------- */

static bool on_item(const void *obj, uint32_t len, bool trunc, void *user) {
    ctx_t *c = user;
    if (trunc) return true; /* skip */
    /* Simulate processing cost. */
    (void)obj; (void)len;
    atomic_fetch_add(&c->consumed, 1);
    return true;
}

/* ---------------- main ---------------- */

int main(void) {
    void *rb_mem  = aligned_or_die(RB_CACHE_LINE, rb_size(CAPACITY));
    void *scratch = aligned_or_die(RB_CACHE_LINE, (size_t)SLOTS * SLOT_SIZE);

    rb_t *rb = (rb_t *)rb_mem;

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = CAPACITY;
    cfg.slots     = SLOTS;
    cfg.slot_size = SLOT_SIZE;

    if (rb_init(rb, &cfg, scratch, SLOTS * SLOT_SIZE) != RB_OK) {
        fprintf(stderr, "rb_init failed\n");
        return 1;
    }

    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rb = rb;
    atomic_init(&ctx.produced, 0);
    atomic_init(&ctx.consumed, 0);
    atomic_init(&ctx.producer_done, false);

    rb_thread_t t;
    if (rb_thread_spawn(rb, &t, true, producer_fn, &ctx) != RB_OK) {
        fprintf(stderr, "spawn failed\n");
        return 1;
    }

    printf("notification example: consumer blocks in rb_wait between bursts\n");

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t wakeups = 0;
    uint64_t idle_timeouts = 0;

    for (;;) {
        /* Snapshot head BEFORE draining. See header comment. */
        uint32_t v = rb_notify_value(rb);

        uint32_t n = rb_drain(rb, on_item, &ctx);
        if (n > 0) continue;

        if (atomic_load(&ctx.producer_done)) break;

        /* Nothing to do. Block until the producer publishes.
           100 ms timeout lets us notice producer_done promptly. */
        rb_err_t e = rb_wait(rb, v, 100);
        if (e == RB_OK)      wakeups++;
        else                 idle_timeouts++;
    }

    rb_thread_join(&t);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("done: produced=%llu consumed=%llu in %.2fs\n",
           (unsigned long long)atomic_load(&ctx.produced),
           (unsigned long long)atomic_load(&ctx.consumed),
           secs);
    printf("wakeups=%llu idle_timeouts=%llu (timeouts = ~%.0f%% idle)\n",
           (unsigned long long)wakeups,
           (unsigned long long)idle_timeouts,
           100.0 * (double)idle_timeouts / (double)(wakeups + idle_timeouts));

    rb_deinit(rb);
    free(scratch);
    free(rb_mem);
    return 0;
}
