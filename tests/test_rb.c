/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

#define _POSIX_C_SOURCE 200809L

#include "rb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- tiny harness ---------------- */

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg) do {                                             \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
        g_failures++;                                                     \
    }                                                                     \
} while (0)

static void *aligned_alloc_or_die(size_t align, size_t size) {
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0 || !p) {
        fprintf(stderr, "posix_memalign(%zu, %zu) failed\n", align, size);
        exit(2);
    }
    return p;
}

/* ---------------- fixture ---------------- */

typedef struct {
    rb_t  *rb;
    void  *rb_mem;
    void  *scratch;
    size_t scratch_size;
} fixture_t;

static uint32_t stride_for(uint32_t slot_size) {
#if RB_SLOT_CACHELINE_PAD
    size_t a = RB_CACHE_LINE;
#else
    size_t a = _Alignof(max_align_t);
#endif
    if (a < 4) a = 4;
    return (uint32_t)((slot_size + a - 1u) & ~(a - 1u));
}

static fixture_t make_fixture(uint32_t cap, uint32_t slots, uint32_t slot_size,
                              const rb_callbacks_t *cb)
{
    fixture_t f;
    memset(&f, 0, sizeof f);

    size_t rb_bytes = rb_size(cap);
    f.rb_mem = aligned_alloc_or_die(RB_CACHE_LINE, rb_bytes);
    f.rb     = (rb_t *)f.rb_mem;

    uint32_t stride = stride_for(slot_size);
    f.scratch_size = (size_t)slots * (size_t)stride;
    f.scratch = aligned_alloc_or_die(RB_CACHE_LINE, f.scratch_size);

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = cap;
    cfg.slots     = slots;
    cfg.slot_size = slot_size;
    if (cb) cfg.cb = *cb;

    rb_err_t e = rb_init(f.rb, &cfg, f.scratch, f.scratch_size);
    if (e != RB_OK) {
        fprintf(stderr, "rb_init failed: %d\n", (int)e);
        exit(2);
    }
    return f;
}

static void free_fixture(fixture_t *f) {
    rb_deinit(f->rb);
    free(f->scratch);
    free(f->rb_mem);
    f->scratch = f->rb_mem = NULL;
    f->rb = NULL;
}

/* ---------------- tests ---------------- */

static void test_init_validation(void) {
    rb_config_t cfg;
    rb_config_init(&cfg);

    void *rb_mem  = aligned_alloc_or_die(RB_CACHE_LINE, rb_size(64));
    void *scratch = aligned_alloc_or_die(RB_CACHE_LINE, 64 * 2048);

    rb_config_set_capacity(&cfg, 48); /* not power of two */
    CHECK(rb_init(rb_mem, &cfg, scratch, 64*2048) == RB_ERR_INVAL,
          "non-pow2 capacity rejected");

    rb_config_init(&cfg);
    rb_config_set_capacity(&cfg, 64);
    rb_config_set_slots(&cfg, 64);
    rb_config_set_slot_size(&cfg, 4); /* <= header */
    CHECK(rb_init(rb_mem, &cfg, scratch, 64*2048) == RB_ERR_INVAL,
          "slot_size <= header rejected");

    rb_config_init(&cfg);
    rb_config_set_capacity(&cfg, 64);
    rb_config_set_slots(&cfg, 64);
    rb_config_set_slot_size(&cfg, 2048);
    CHECK(rb_init(rb_mem, &cfg, scratch, 10) == RB_ERR_INVAL,
          "too-small scratchpad rejected");

    free(scratch);
    free(rb_mem);
}

static void test_basic_roundtrip(void) {
    fixture_t f = make_fixture(8, 8, 128, NULL);

    CHECK(rb_count(f.rb) == 0, "initial empty");
    CHECK(rb_is_empty(f.rb),   "is_empty initial");

    uint32_t idx, cap;
    void *w;
    CHECK(rb_acquire(f.rb, 10, &idx, &w, &cap) == RB_OK, "acquire ok");
    memcpy(w, "hello", 5);
    CHECK(rb_publish(f.rb, idx, 5) == RB_OK, "publish ok");
    CHECK(rb_count(f.rb) == 1, "count==1 after publish");

    uint32_t cidx, clen;
    const void *obj;
    bool trunc;
    CHECK(rb_consume(f.rb, &cidx, &obj, &clen, &trunc) == RB_OK, "consume ok");
    CHECK(cidx == idx, "same slot index");
    CHECK(clen == 5,   "len 5");
    CHECK(!trunc,      "not truncated");
    CHECK(memcmp(obj, "hello", 5) == 0, "payload matches");
    CHECK(rb_release(f.rb, cidx) == RB_OK, "release ok");
    CHECK(rb_is_empty(f.rb), "empty after release");

    free_fixture(&f);
}

static void test_full_and_empty(void) {
    fixture_t f = make_fixture(4, 4, 64, NULL);

    for (int i = 0; i < 4; ++i) {
        uint32_t idx, cap; void *w;
        CHECK(rb_acquire(f.rb, 1, &idx, &w, &cap) == RB_OK, "acquire in loop");
        ((uint8_t *)w)[0] = (uint8_t)i;
        CHECK(rb_publish(f.rb, idx, 1) == RB_OK, "publish in loop");
    }
    CHECK(rb_is_full(f.rb), "is_full after 4");

    uint32_t idx, cap; void *w;
    CHECK(rb_acquire(f.rb, 1, &idx, &w, &cap) == RB_ERR_FULL,
          "acquire on full returns FULL");

    uint32_t cidx, clen; const void *obj; bool trunc;
    CHECK(rb_consume(f.rb, &cidx, &obj, &clen, &trunc) == RB_OK, "consume");
    CHECK(rb_release(f.rb, cidx) == RB_OK, "release");

    CHECK(rb_acquire(f.rb, 1, &idx, &w, &cap) == RB_OK, "acquire after release");

    free_fixture(&f);
}

static void test_truncate(void) {
    fixture_t f = make_fixture(4, 4, 32, NULL); /* payload cap = 32 - RB_SLOT_HDR_SIZE */
    uint32_t idx, cap; void *w;
    CHECK(rb_acquire(f.rb, 100, &idx, &w, &cap) == RB_OK, "acquire oversize ok");
    CHECK(cap == 32u - RB_SLOT_HDR_SIZE, "cap is slot_size - RB_SLOT_HDR_SIZE");
    memset(w, 0xAA, cap);
    CHECK(rb_publish(f.rb, idx, cap) == RB_OK, "publish truncated");

    uint32_t cidx, clen; const void *obj; bool trunc;
    CHECK(rb_consume(f.rb, &cidx, &obj, &clen, &trunc) == RB_OK, "consume");
    CHECK(clen == 32u - RB_SLOT_HDR_SIZE, "len is cap");
    CHECK(trunc,      "TRUNCATED flag set");
    rb_release(f.rb, cidx);

#if RB_ENABLE_STATS
    CHECK(rb_stats(f.rb)->truncated == 1, "stats.truncated == 1");
#endif

    free_fixture(&f);
}

static void test_drop_policy(void) {
    fixture_t f = make_fixture(4, 4, 32, NULL);
    CHECK(rb_set_oversize_policy(f.rb, RB_OVERSIZE_DROP) == RB_OK, "set drop");

    uint32_t idx, cap; void *w;
    CHECK(rb_acquire(f.rb, 100, &idx, &w, &cap) == RB_ERR_OVERSIZE,
          "DROP rejects oversize");
    CHECK(rb_count(f.rb) == 0, "no slot taken under DROP");

    free_fixture(&f);
}

/* ---------------- callbacks ---------------- */

static int cb_added, cb_full, cb_low_d, cb_low_e;
static uint32_t cb_last_idx, cb_last_len;
static bool cb_last_trunc;

static void on_added(rb_t *rb, uint32_t idx, uint32_t len,
                     bool trunc, void *user) {
    (void)rb; (void)user;
    cb_added++;
    cb_last_idx = idx;
    cb_last_len = len;
    cb_last_trunc = trunc;
}
static void on_full(rb_t *rb, void *user)  { (void)rb; (void)user; cb_full++;  }
static void on_low_d(rb_t *rb, void *user) { (void)rb; (void)user; cb_low_d++; }
static void on_low_e(rb_t *rb, void *user) { (void)rb; (void)user; cb_low_e++; }

static void test_callbacks(void) {
    rb_callbacks_t cb;
    memset(&cb, 0, sizeof cb);
    cb.on_slot_added = on_added;
    cb.on_full       = on_full;
    cb.on_low_d      = on_low_d;
    cb.on_low_e      = on_low_e;

    fixture_t f = make_fixture(16, 16, 64, &cb);
    CHECK(rb_set_low_d(f.rb, 50) == RB_OK, "low_d 50");
    CHECK(rb_set_low_e(f.rb, 25) == RB_OK, "low_e 25");
    /* limit = min(capacity, slots) = 16 */

    cb_added = cb_full = cb_low_d = cb_low_e = 0;

    /* Fill to exactly limit -> on_full should fire once. */
    for (int i = 0; i < 16; ++i) {
        uint32_t idx, cap; void *w;
        CHECK(rb_acquire(f.rb, 4, &idx, &w, &cap) == RB_OK, "acquire");
        memcpy(w, "abcd", 4);
        CHECK(rb_publish(f.rb, idx, 4) == RB_OK, "publish");
    }
    CHECK(cb_added == 16, "on_slot_added fired 16 times");
    CHECK(cb_full  == 1,  "on_full fired once");

    /* Drain 9 -> count 7. 16 * 50% = 8. crossed 8 -> fire d once. */
    for (int i = 0; i < 9; ++i) {
        uint32_t cidx, clen; const void *obj; bool tr;
        CHECK(rb_consume(f.rb, &cidx, &obj, &clen, &tr) == RB_OK, "consume");
        CHECK(rb_release(f.rb, cidx) == RB_OK, "release");
    }
    CHECK(cb_low_d == 1, "on_low_d fired once");
    CHECK(cb_low_e == 0, "on_low_e not yet");

    /* Drain to 3. 16 * 25% = 4. crossed 4 -> fire e once. */
    for (int i = 0; i < 4; ++i) {
        uint32_t cidx, clen; const void *obj; bool tr;
        CHECK(rb_consume(f.rb, &cidx, &obj, &clen, &tr) == RB_OK, "consume");
        CHECK(rb_release(f.rb, cidx) == RB_OK, "release");
    }
    CHECK(cb_low_e == 1, "on_low_e fired once");

    free_fixture(&f);
}

/* ---------------- drain / flush ---------------- */

static uint32_t drain_sum;
static bool drain_cb(const void *obj, uint32_t len, bool tr, void *user) {
    (void)user; (void)tr;
    for (uint32_t i = 0; i < len; ++i) drain_sum += ((const uint8_t *)obj)[i];
    return true;
}

static void test_drain(void) {
    fixture_t f = make_fixture(8, 8, 64, NULL);
    for (int i = 0; i < 4; ++i) {
        uint32_t idx, cap; void *w;
        rb_acquire(f.rb, 1, &idx, &w, &cap);
        ((uint8_t *)w)[0] = 10;
        rb_publish(f.rb, idx, 1);
    }
    drain_sum = 0;
    uint32_t n = rb_drain(f.rb, drain_cb, NULL);
    CHECK(n == 4, "drained 4");
    CHECK(drain_sum == 40, "sum == 40");
    CHECK(rb_is_empty(f.rb), "empty after drain");
    free_fixture(&f);
}

typedef struct { int remaining; } flush_state;

static bool flush_cb(void *w, uint32_t cap, uint32_t *out_len,
                     bool *out_trunc, bool *out_stop, void *user) {
    flush_state *s = (flush_state *)user;
    (void)cap;
    if (s->remaining == 0) { *out_stop = true; *out_len = 0; return true; }
    ((uint8_t *)w)[0] = (uint8_t)s->remaining;
    *out_len = 1;
    *out_trunc = false;
    *out_stop = false;
    s->remaining--;
    return true;
}

static void test_flush(void) {
    fixture_t f = make_fixture(8, 8, 64, NULL);
    flush_state s = { .remaining = 3 };
    uint32_t n = rb_flush(f.rb, flush_cb, &s);
    CHECK(n == 3, "flushed 3");
    CHECK(rb_count(f.rb) == 3, "count == 3");
    free_fixture(&f);
}

/* ---------------- runtime setters ---------------- */

static void test_runtime_setters(void) {
    fixture_t f = make_fixture(16, 16, 64, NULL);

    CHECK(rb_set_limit(f.rb, 0)  == RB_ERR_INVAL, "limit 0 rejected");
    CHECK(rb_set_limit(f.rb, 99) == RB_ERR_INVAL, "limit > capacity rejected");
    CHECK(rb_set_limit(f.rb, 8)  == RB_OK,        "limit 8 accepted");
    CHECK(rb_limit(f.rb) == 8, "limit is 8");

    CHECK(rb_set_low_d(f.rb, 101) == RB_ERR_INVAL, "low_d > 100 rejected");
    CHECK(rb_set_low_e(f.rb, 101) == RB_ERR_INVAL, "low_e > 100 rejected");

    /* limit cannot change while non-empty */
    uint32_t idx, cap; void *w;
    rb_acquire(f.rb, 1, &idx, &w, &cap);
    ((uint8_t *)w)[0] = 1;
    rb_publish(f.rb, idx, 1);
    CHECK(rb_set_limit(f.rb, 4) == RB_ERR_INVAL, "limit change while non-empty rejected");

    /* stack setters */
    CHECK(rb_set_producer_stack(f.rb, 512u * 1024u) == RB_OK, "producer stack");
    CHECK(rb_set_consumer_stack(f.rb, 512u * 1024u) == RB_OK, "consumer stack");

    free_fixture(&f);
}

/* ---------------- stats ---------------- */

static void test_stats(void) {
    fixture_t f = make_fixture(8, 8, 64, NULL);
    uint32_t idx, cap; void *w;
    rb_acquire(f.rb, 1, &idx, &w, &cap);
    ((uint8_t *)w)[0] = 1;
    rb_publish(f.rb, idx, 1);
    rb_acquire(f.rb, 1, &idx, &w, &cap);
    ((uint8_t *)w)[0] = 2;
    rb_publish(f.rb, idx, 1);

    uint32_t cidx, clen; const void *obj; bool tr;
    rb_consume(f.rb, &cidx, &obj, &clen, &tr);
    rb_release(f.rb, cidx);

#if RB_ENABLE_STATS
    const rb_stats_t *st = rb_stats(f.rb);
    CHECK(st->published == 2, "stats published");
    CHECK(st->consumed  == 1, "stats consumed");
    CHECK(st->high_water == 2, "high water");
    rb_stats_reset(f.rb);
    CHECK(rb_stats(f.rb)->published == 0, "stats reset");
#endif
    free_fixture(&f);
}

/* ---------------- mis-pairing must be rejected ---------------- */

static void test_mispair(void) {
    fixture_t f = make_fixture(4, 4, 64, NULL);

    uint32_t idx, cap; void *w;
    CHECK(rb_acquire(f.rb, 1, &idx, &w, &cap) == RB_OK, "acquire");
    CHECK(rb_acquire(f.rb, 1, &idx, &w, &cap) == RB_ERR_INVAL,
          "double acquire rejected");

    /* abort path */
    CHECK(rb_abort(f.rb) == RB_OK, "abort ok");
    CHECK(rb_acquire(f.rb, 1, &idx, &w, &cap) == RB_OK, "acquire after abort");
    ((uint8_t *)w)[0] = 7;
    CHECK(rb_publish(f.rb, idx, 1) == RB_OK, "publish");

    uint32_t cidx, clen; const void *obj; bool tr;
    CHECK(rb_consume(f.rb, &cidx, &obj, &clen, &tr) == RB_OK, "consume");
    CHECK(rb_consume(f.rb, &cidx, &obj, &clen, &tr) == RB_ERR_INVAL,
          "double consume rejected");
    CHECK(rb_release(f.rb, cidx) == RB_OK, "release");

    free_fixture(&f);
}

static void test_latched_callbacks(void) {
    rb_callbacks_t cb;
    memset(&cb, 0, sizeof cb);
    cb.on_slot_added = on_added;
    cb.on_full       = on_full;
    cb.on_low_d      = on_low_d;
    cb.on_low_e      = on_low_e;

    fixture_t f = make_fixture(16, 16, 64, &cb);
    rb_set_low_d(f.rb, 50);   /* threshold at count 8 */
    rb_set_low_e(f.rb, 25);   /* threshold at count 4 */

    cb_added = cb_full = cb_low_d = cb_low_e = 0;

    /* Phase 1: fill to limit -> on_full fires exactly once. */
    for (int i = 0; i < 16; ++i) {
        uint32_t idx, cap; void *w;
        rb_acquire(f.rb, 1, &idx, &w, &cap);
        ((uint8_t *)w)[0] = (uint8_t)i;
        rb_publish(f.rb, idx, 1);
    }
    CHECK(cb_full == 1, "fill to limit: on_full once");
    CHECK(cb_added == 16, "on_slot_added fires per publish");

    /* Phase 2: drain one (count 15), refill (count 16). on_full must NOT
       re-fire because full_latch is still set. */
    {
        uint32_t idx, len; const void *o; bool tr;
        rb_consume(f.rb, &idx, &o, &len, &tr);
        rb_release(f.rb, idx);
    }
    CHECK(cb_low_d == 0, "count 15 still above 50%");
    {
        uint32_t idx, cap; void *w;
        rb_acquire(f.rb, 1, &idx, &w, &cap);
        ((uint8_t *)w)[0] = 200;
        rb_publish(f.rb, idx, 1);
    }
    CHECK(cb_full == 1, "top of cycle: on_full does NOT re-fire");

    /* Phase 3: drain 9 -> count 16->7, crosses 50% at 8->7. */
    for (int i = 0; i < 9; ++i) {
        uint32_t idx, len; const void *o; bool tr;
        rb_consume(f.rb, &idx, &o, &len, &tr);
        rb_release(f.rb, idx);
    }
    CHECK(cb_low_d == 1, "crossing below d: on_low_d once");

    /* Phase 4: drain 4 more -> count 7->3, crosses 25% at 4->3. */
    for (int i = 0; i < 4; ++i) {
        uint32_t idx, len; const void *o; bool tr;
        rb_consume(f.rb, &idx, &o, &len, &tr);
        rb_release(f.rb, idx);
    }
    CHECK(cb_low_e == 1, "crossing below e: on_low_e once");

    /* Phase 5: refill from 3 to 16 -> on_full fires once more, because
       low_d in phase 3 cleared full_latch. */
    for (int i = 0; i < 13; ++i) {
        uint32_t idx, cap; void *w;
        rb_acquire(f.rb, 1, &idx, &w, &cap);
        ((uint8_t *)w)[0] = (uint8_t)(100 + i);
        rb_publish(f.rb, idx, 1);
    }
    CHECK(cb_full == 2, "after full drain+refill: on_full re-fires");

    free_fixture(&f);
}

/* ---------------- config override of defaults ---------------- */

static void test_config_override(void) {
    rb_config_t cfg;
    rb_config_init(&cfg);
    CHECK(cfg.capacity == RB_CAPACITY,   "default capacity");
    CHECK(cfg.slots    == RB_NUM_SLOTS,  "default slots");
    CHECK(cfg.slot_size== RB_SLOT_SIZE,  "default slot size");

    rb_config_set_capacity(&cfg, 16);
    rb_config_set_slots(&cfg, 16);
    rb_config_set_slot_size(&cfg, 128);
    rb_config_set_low_d(&cfg, 60);
    rb_config_set_low_e(&cfg, 30);

    void *rb_mem  = aligned_alloc_or_die(RB_CACHE_LINE, rb_size(16));
    void *scratch = aligned_alloc_or_die(RB_CACHE_LINE, 16 * stride_for(128));
    CHECK(rb_init(rb_mem, &cfg, scratch, 16 * stride_for(128)) == RB_OK,
          "init with overrides");
    CHECK(rb_capacity(rb_mem) == 16, "capacity override");
    CHECK(rb_limit(rb_mem)    == 16, "limit defaults to min(cap, slots)");
    rb_deinit(rb_mem);
    free(scratch);
    free(rb_mem);
}

/* ---------------- main ---------------- */

int main(void) {
    test_init_validation();
    test_basic_roundtrip();
    test_full_and_empty();
    test_truncate();
    test_drop_policy();
    test_callbacks();
    test_latched_callbacks();
    test_drain();
    test_flush();
    test_runtime_setters();
    test_stats();
    test_mispair();
    test_config_override();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
