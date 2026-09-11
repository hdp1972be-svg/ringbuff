/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

/*
 * rb.c — SPSC ring buffer of references into a caller-owned scratchpad.
 *
 * ── Overview ─────────────────────────────────────────────────────────
 *
 * The ring holds small entries (uint32_t slot indices by default, void* with RB_USE_POINTERS=1). The payloads live 
 * in a separate, fixed-stride scratchpad provided by the caller. A slot's bytes never move through  * the ring; only 
 * its index does. Producer writes directly into a slot, * publishes the index, and moves on. Consumer reads directly 
 * from the * same slot, then releases it. Zero-copy in both directions.
 *
 *   ring control block          scratchpad (caller-owned)
 *   +------------------+        +-------------------------+
 *   | head  (atomic)   |        | slot 0: [hdr][payload]  |
 *   | cached_tail      |        | slot 1: [hdr][payload]  |
 *   | full_latch       |        | ...                     |
 *   |------------------|        | slot z-1: [hdr][payload]|
 *   | tail  (atomic)   |        +-------------------------+
 *   | cached_head      |
 *   | low_*_latch      |        entries[] holds indices into scratch
 *   |------------------|
 *   | capacity, limit  |
 *   | slots, slot_size |
 *   | callbacks, stats |
 *   |------------------|
 *   | entries[cap]     |
 *   +------------------+
 *
 * head and tail live on separate cache lines: the producer only ever writes head, the consumer only ever writes tail. 
 * Each side caches the * other's counter in cached_* to avoid an atomic load per operation.
 *
 * ── Invariants ───────────────────────────────────────────────────────
 *
 *   SPSC.               Exactly one producer thread and one consumer thread. Not MPSC, not MPMC.
 *
 *   Pairing.            Each rb_acquire is followed by exactly one of rb_publish, rb_publish_ex, or rb_abort, from
 *                       the same thread, before the next acquire. Each rb_consume is followed by exactly one rb_
 *                       release, from the same thread, before the next consume. Violations return RB_ERR_INVAL.
 *
 *   Release order.      The consumer releases slots in FIFO order. This removes the need for a free-list ring: the
 *                       producer simply takes `head & slots_mask` at acquire time. Out-of-order release is not
 *                       supported.
 *
 *   Never overwrite.    A producer never writes past a slot boundary. Under RB_OVERSIZE_TRUNCATE, the write is clamped
 *                       to capacity and the TRUNCATED bit is set in the header. Under RB_OVERSIZE_DROP, the acquire
 *                       fails with RB_ERR_OVERSIZE and no slot is consumed.
 *
 *   Frozen layout.      capacity, slots, and slot_size are set at rb_init and never change. `limit` may be lowered
 *                       at runtime only while the ring is empty.
 *
 *   No allocation.      This file never calls malloc. rb_size() tells the caller how much to allocate; rb_init()
 *                       installs caller-provided memory.
 *
 * ── Memory ordering ──────────────────────────────────────────────────
 *
 *   The publish path is: write the payload and header, store the entry into entries[head & mask], then store head 
 *   with release ordering. The consume path is: load head with acquire ordering, read the entry, read the header 
 *   and payload. That pairing makes the payload and header visible to the consumer before the new head value is,
 *   without a full barrier on either side. full_latch is written by the producer and cleared by the consumer,
 *   so it is atomic with relaxed ordering. Worst case under relaxed ordering is one extra on_full fire 
 *   — over-notification, never under. The low_*_latch bits are touched by the consumer only and are plain uint32_t.
 *
 *   RB_SINGLE_THREADED=1 compiles all of the above down to plain loads and stores. No atomics, no barriers, no 
 *   per-op cost. Suitable for an ISR-driven producer with a main-loop consumer, or for a single event loop driving 
 *   both sides.
 *
 * ── Callbacks ────────────────────────────────────────────────────────
 *
 *   All four callbacks (on_slot_added, on_full, on_low_d, on_low_e) are informational. They never gate control flow, 
 *   never block, and must not re-enter the ring. The threshold callbacks are latched: they fire once on entering the 
 *   warning region and not again until the region is exited. The consumer's hot path — rb_drain — does
 *   not consult any callback.
 *
 * ── Portability ──────────────────────────────────────────────────────
 *
 *   The only platform-dependent pieces are in rb_port.h: atomics, barriers, and alignment. This file uses nothing 
 *   else beyond <string.h> for memcpy/memset. No POSIX, no libc beyond that. See rb.h for the public API and 
 *   docs/TECHNICAL.md for the rationale behind the latched callbacks, the FIFO release contract, and the
 *   index-vs-pointer tradeoff.
 */

#include "rb.h"
#include "rb_port.h"
#include <string.h>

#define RB_NO_PENDING 0xFFFFFFFFu

/* ---------------- control block ---------------- */
struct rb_s {
    /* producer line */
    RB_ALIGNAS(RB_CACHE_LINE) rb_atomic_u32 head;
    uint32_t cached_tail;
    uint32_t pending_slot;   /* RB_NO_PENDING when idle */
    uint32_t pending_wanted;
    rb_atomic_u32 full_latch;        /* <-- ADD */

    /* consumer line */
    RB_ALIGNAS(RB_CACHE_LINE) rb_atomic_u32 tail;
    uint32_t cached_head;
    uint32_t consumer_active;
    uint32_t consumer_slot;
    uint32_t low_d_latch;       /* <-- ADD */
    uint32_t low_e_latch;       /* <-- ADD */

    /* cold config */
    uint32_t capacity;
    uint32_t limit;
    uint32_t mask;           /* capacity - 1 */
    uint32_t slots;
    uint32_t slots_mask;     /* slots - 1 if pow2, else 0 */
    uint32_t slot_size;
    uint32_t slot_stride;
    uint32_t low_d;
    uint32_t low_e;
    rb_oversize_policy_t oversize_policy;
    size_t   producer_stack_size;
    size_t   consumer_stack_size;
    uint8_t *scratch;
    size_t   scratch_size;
    rb_callbacks_t cb;

#if RB_ENABLE_STATS
    rb_stats_t stats;
#endif

    rb_entry_t entries[];    /* flexible array member */
};

/* ---------------- helpers ---------------- */

static inline uint32_t slot_stride_for(uint32_t slot_size) {
#if RB_SLOT_CACHELINE_PAD
    size_t a = RB_CACHE_LINE;
#else
    size_t a = _Alignof(max_align_t);
#endif
    if (a < 4) a = 4;
    return (uint32_t)RB_ALIGN_UP(slot_size, a);
}

static inline bool below_low(uint32_t count, uint32_t limit, uint32_t percent) {
    if (percent == 0u) return false;
    return (uint64_t)count * 100u < (uint64_t)limit * percent;
}

static inline uint32_t slot_index_for(const rb_t *rb, uint32_t head) {
    return rb->slots_mask ? (head & rb->slots_mask) : (head % rb->slots);
}

/* ---------------- config API ---------------- */

void rb_config_init(rb_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->capacity            = RB_CAPACITY;
    cfg->limit               = 0;
    cfg->slots               = RB_NUM_SLOTS;
    cfg->slot_size           = RB_SLOT_SIZE;
    cfg->low_d               = RB_DEFAULT_LOW_D;
    cfg->low_e               = RB_DEFAULT_LOW_E;
    cfg->oversize_policy     = RB_OVERSIZE_TRUNCATE;
    cfg->producer_stack_size = 0;
    cfg->consumer_stack_size = 0;
    cfg->cb.on_slot_added    = NULL;
    cfg->cb.on_low_d         = NULL;
    cfg->cb.on_low_e         = NULL;
    cfg->cb.on_full          = NULL;
    cfg->cb.user             = NULL;
}

void rb_config_set_capacity(rb_config_t *c, uint32_t v) { if (c) c->capacity = v; }
void rb_config_set_limit   (rb_config_t *c, uint32_t v) { if (c) c->limit = v; }
void rb_config_set_slots   (rb_config_t *c, uint32_t v) { if (c) c->slots = v; }
void rb_config_set_slot_size(rb_config_t *c, uint32_t v) { if (c) c->slot_size = v; }
void rb_config_set_low_d   (rb_config_t *c, uint32_t v) { if (c) c->low_d = v; }
void rb_config_set_low_e   (rb_config_t *c, uint32_t v) { if (c) c->low_e = v; }
void rb_config_set_oversize_policy(rb_config_t *c, rb_oversize_policy_t p) {
    if (c) c->oversize_policy = p;
}
void rb_config_set_callbacks(rb_config_t *c, const rb_callbacks_t *cb) {
    if (c && cb) c->cb = *cb;
}
void rb_config_set_producer_stack(rb_config_t *c, size_t v) {
    if (c) c->producer_stack_size = v;
}
void rb_config_set_consumer_stack(rb_config_t *c, size_t v) {
    if (c) c->consumer_stack_size = v;
}

/* ---------------- lifecycle ---------------- */

size_t rb_size(uint32_t capacity) {
    return offsetof(rb_t, entries) + (size_t)capacity * sizeof(rb_entry_t);
}

size_t rb_producer_stack(const rb_t *rb) {
    return rb ? rb->producer_stack_size : 0u;
}

size_t rb_consumer_stack(const rb_t *rb) {
    return rb ? rb->consumer_stack_size : 0u;
}

rb_err_t rb_init(rb_t *rb, const rb_config_t *cfg,
                 void *scratch, size_t scratch_size)
{
    if (!rb || !cfg || !scratch) return RB_ERR_INVAL;
    if (((uintptr_t)rb % _Alignof(rb_t)) != 0) return RB_ERR_INVAL;

    uint32_t capacity = cfg->capacity ? cfg->capacity : RB_CAPACITY;
    if (!RB_IS_POW2(capacity) || capacity < 2u) return RB_ERR_INVAL;

    uint32_t slots = cfg->slots ? cfg->slots : RB_NUM_SLOTS;
    if (slots == 0u) return RB_ERR_INVAL;

    uint32_t slot_size = cfg->slot_size ? cfg->slot_size : RB_SLOT_SIZE;
    if (slot_size <= RB_SLOT_HDR_SIZE) return RB_ERR_INVAL;

    uint32_t limit = cfg->limit ? cfg->limit : capacity;
    if (limit > capacity) limit = capacity;
    if (limit > slots)    limit = slots;
    if (limit == 0u)      return RB_ERR_INVAL;

    if (cfg->low_d > 100u || cfg->low_e > 100u) return RB_ERR_INVAL;
    if (cfg->low_e > cfg->low_d) return RB_ERR_INVAL;

#if RB_SLOT_CACHELINE_PAD
    size_t need_align = RB_CACHE_LINE;
#else
    size_t need_align = _Alignof(max_align_t);
#endif
    if (need_align < 4u) need_align = 4u;
    if (((uintptr_t)scratch % need_align) != 0u) return RB_ERR_INVAL;

    uint32_t stride = slot_stride_for(slot_size);
    size_t   need   = (size_t)slots * (size_t)stride;
    if (scratch_size < need) return RB_ERR_INVAL;

    memset(rb, 0, offsetof(rb_t, entries));

    rb->capacity            = capacity;
    rb->limit               = limit;
    rb->mask                = capacity - 1u;
    rb->slots               = slots;
    rb->slots_mask          = RB_IS_POW2(slots) ? (slots - 1u) : 0u;
    rb->slot_size           = slot_size;
    rb->slot_stride         = stride;
    rb->low_d               = cfg->low_d;
    rb->low_e               = cfg->low_e;
    rb->oversize_policy     = cfg->oversize_policy;
    rb->producer_stack_size = cfg->producer_stack_size
                              ? cfg->producer_stack_size
                              : RB_DEFAULT_PRODUCER_STACK;
    rb->consumer_stack_size = cfg->consumer_stack_size
                              ? cfg->consumer_stack_size
                              : RB_DEFAULT_CONSUMER_STACK;
    rb->scratch             = (uint8_t *)scratch;
    rb->scratch_size        = scratch_size;
    rb->cb                  = cfg->cb;

    RB_ATOMIC_STORE_REL(&rb->head, 0u);
    RB_ATOMIC_STORE_REL(&rb->tail, 0u);
    rb->cached_tail   = 0u;
    rb->cached_head   = 0u;
    rb->pending_slot  = RB_NO_PENDING;
    rb->pending_wanted = 0u;
    rb->consumer_active = 0u;
    rb->consumer_slot   = 0u;
    RB_ATOMIC_STORE_RLX(&rb->full_latch, 0u);
    rb->low_d_latch     = 0u;   /* <-- ADD */
    rb->low_e_latch     = 0u;   /* <-- ADD */

#if RB_ENABLE_STATS
    memset(&rb->stats, 0, sizeof rb->stats);
#endif

    return RB_OK;
}

void rb_deinit(rb_t *rb) {
    if (!rb) return;
    rb->scratch         = NULL;
    rb->scratch_size    = 0;
    rb->pending_slot    = RB_NO_PENDING;
    rb->consumer_active = 0u;
}

/* ---------------- producer ---------------- */

rb_err_t rb_acquire(rb_t *rb, uint32_t wanted_len,
                    uint32_t *out_slot_index,
                    void **out_writable, uint32_t *out_cap)
{
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (rb->pending_slot != RB_NO_PENDING) return RB_ERR_INVAL;

    uint32_t head  = RB_ATOMIC_LOAD_RLX(&rb->head);
    uint32_t tail  = rb->cached_tail;
    uint32_t count = head - tail;

    if (count >= rb->limit) {
        tail = RB_ATOMIC_LOAD_ACQ(&rb->tail);
        rb->cached_tail = tail;
        count = head - tail;
        if (count >= rb->limit) {
#if RB_ENABLE_STATS
            rb->stats.full_attempts++;
#endif
            return RB_ERR_FULL;
        }
    }

    uint32_t cap = rb->slot_size - RB_SLOT_HDR_SIZE;
    if (wanted_len > 0u && wanted_len > cap
        && rb->oversize_policy == RB_OVERSIZE_DROP) {
        return RB_ERR_OVERSIZE;
    }

    uint32_t slot_index = slot_index_for(rb, head);

    rb->pending_slot   = slot_index;
    rb->pending_wanted = wanted_len;

    if (out_slot_index) *out_slot_index = slot_index;
    if (out_writable) {
        uint8_t *slot = rb->scratch + (size_t)slot_index * rb->slot_stride;
        *out_writable = slot + RB_SLOT_HDR_SIZE;
    }
    if (out_cap) *out_cap = cap;
    return RB_OK;
}

rb_err_t rb_publish_ex(rb_t *rb, uint32_t slot_index,
                       uint32_t written_len, bool truncated)
{
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (rb->pending_slot == RB_NO_PENDING) return RB_ERR_INVAL;
    if (rb->pending_slot != slot_index)    return RB_ERR_INVAL;

    /* Auto-truncate if caller said more than fits. */
    uint32_t cap = rb->slot_size - RB_SLOT_HDR_SIZE;
    if (written_len > cap) {
        written_len = cap;
        truncated   = true;
    }
    if (rb->pending_wanted > cap) truncated = true;

    uint32_t hdr = written_len & RB_SLOT_LEN_MASK;
    if (truncated) hdr |= RB_SLOT_TRUNCATED;

    uint8_t *slot = rb->scratch + (size_t)slot_index * rb->slot_stride;
    memcpy(slot, &hdr, sizeof hdr);

    uint32_t head = RB_ATOMIC_LOAD_RLX(&rb->head);

#if RB_USE_POINTERS
    rb->entries[head & rb->mask] = (rb_entry_t)slot;
#else
    rb->entries[head & rb->mask] = (rb_entry_t)slot_index;
#endif

    RB_ATOMIC_STORE_REL(&rb->head, head + 1u);

    rb->pending_slot   = RB_NO_PENDING;
    rb->pending_wanted = 0u;

    uint32_t tail  = RB_ATOMIC_LOAD_ACQ(&rb->tail);
    rb->cached_tail = tail;
    uint32_t count = (head + 1u) - tail;

#if RB_ENABLE_STATS
    rb->stats.published++;
    if (count > rb->stats.high_water) rb->stats.high_water = count;
    if (truncated) rb->stats.truncated++;
#endif

    if (rb->cb.on_slot_added) {
        rb->cb.on_slot_added(rb, slot_index, written_len, truncated, rb->cb.user);
    }

    if (count >= rb->limit && RB_ATOMIC_LOAD_RLX(&rb->full_latch) == 0u) {
        RB_ATOMIC_STORE_RLX(&rb->full_latch, 1u);
#if RB_ENABLE_STATS
        rb->stats.full_hits++;
#endif
        if (rb->cb.on_full) rb->cb.on_full(rb, rb->cb.user);
    }

    return RB_OK;
}

rb_err_t rb_publish(rb_t *rb, uint32_t slot_index, uint32_t written_len) {
    return rb_publish_ex(rb, slot_index, written_len, false);
}

rb_err_t rb_abort(rb_t *rb) {
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (rb->pending_slot == RB_NO_PENDING) return RB_ERR_INVAL;
    rb->pending_slot   = RB_NO_PENDING;
    rb->pending_wanted = 0u;
    return RB_OK;
}

/* ---------------- consumer ---------------- */

rb_err_t rb_consume(rb_t *rb, uint32_t *out_slot_index,
                    const void **out_obj, uint32_t *out_len,
                    bool *out_truncated)
{
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (rb->consumer_active) return RB_ERR_INVAL;

    uint32_t tail = RB_ATOMIC_LOAD_RLX(&rb->tail);
    uint32_t head = rb->cached_head;

    if (tail == head) {
        head = RB_ATOMIC_LOAD_ACQ(&rb->head);
        rb->cached_head = head;
        if (tail == head) return RB_ERR_EMPTY;
    }

    rb_entry_t entry = rb->entries[tail & rb->mask];

    uint32_t slot_index;
#if RB_USE_POINTERS
    if ((const uint8_t *)entry < rb->scratch) return RB_ERR_INVAL;
    uintptr_t off = (uintptr_t)((const uint8_t *)entry - rb->scratch);
    if (off % rb->slot_stride) return RB_ERR_INVAL;
    slot_index = (uint32_t)(off / rb->slot_stride);
#else
    slot_index = (uint32_t)entry;
#endif
    if (slot_index >= rb->slots) return RB_ERR_INVAL;

    const uint8_t *slot = rb->scratch + (size_t)slot_index * rb->slot_stride;
    uint32_t hdr;
    memcpy(&hdr, slot, sizeof hdr);

    uint32_t len       = hdr & RB_SLOT_LEN_MASK;
    bool     truncated = (hdr & RB_SLOT_TRUNCATED) != 0u;

    rb->consumer_active = 1u;
    rb->consumer_slot   = slot_index;

    if (out_slot_index) *out_slot_index = slot_index;
    if (out_obj)        *out_obj        = slot + RB_SLOT_HDR_SIZE;
    if (out_len)        *out_len        = len;
    if (out_truncated)  *out_truncated  = truncated;
    return RB_OK;
}

rb_err_t rb_release(rb_t *rb, uint32_t slot_index) {
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (!rb->consumer_active) return RB_ERR_INVAL;
    if (slot_index != rb->consumer_slot) return RB_ERR_INVAL;

    uint32_t tail = RB_ATOMIC_LOAD_RLX(&rb->tail);
    RB_ATOMIC_STORE_REL(&rb->tail, tail + 1u);

    rb->consumer_active = 0u;

    uint32_t head      = rb->cached_head;
    uint32_t new_count = head - (tail + 1u);
    uint32_t old_count = new_count + 1u;

#if RB_ENABLE_STATS
    rb->stats.consumed++;
#endif

    bool old_d = below_low(old_count, rb->limit, rb->low_d);
    bool new_d = below_low(new_count, rb->limit, rb->low_d);
    if (!old_d && new_d) {
        rb->low_d_latch = 1u;
        RB_ATOMIC_STORE_RLX(&rb->full_latch, 0u);
#if RB_ENABLE_STATS
        rb->stats.low_d_hits++;
#endif
        if (rb->cb.on_low_d) rb->cb.on_low_d(rb, rb->cb.user);
    } else if (old_d && !new_d) {
        RB_ATOMIC_STORE_RLX(&rb->full_latch, 0u);
    }

    bool old_e = below_low(old_count, rb->limit, rb->low_e);
    bool new_e = below_low(new_count, rb->limit, rb->low_e);
    if (!old_e && new_e) {
        rb->low_e_latch = 1u;
#if RB_ENABLE_STATS
        rb->stats.low_e_hits++;
#endif
        if (rb->cb.on_low_e) rb->cb.on_low_e(rb, rb->cb.user);
    } else if (old_e && !new_e) {
        rb->low_e_latch = 0u;
    }

    return RB_OK;
}

/* ---------------- drain / flush ---------------- */

uint32_t rb_drain(rb_t *rb, rb_drain_fn fn, void *user) {
    if (!rb || !rb->scratch || !fn) return 0u;
    uint32_t n = 0u;
    for (;;) {
        uint32_t idx = 0u, len = 0u;
        const void *obj = NULL;
        bool truncated = false;
        rb_err_t e = rb_consume(rb, &idx, &obj, &len, &truncated);
        if (e != RB_OK) break;
        bool cont = fn(obj, len, truncated, user);
        rb_release(rb, idx);
        n++;
        if (!cont) break;
    }
    return n;
}

uint32_t rb_flush(rb_t *rb, rb_flush_fn fn, void *user) {
    if (!rb || !rb->scratch || !fn) return 0u;
    uint32_t n = 0u;
    for (;;) {
        uint32_t idx = 0u, cap = 0u;
        void *w = NULL;
        rb_err_t e = rb_acquire(rb, 0u, &idx, &w, &cap);
        if (e != RB_OK) break;

        uint32_t len = 0u;
        bool truncated = false, stop = false;
        bool ok = fn(w, cap, &len, &truncated, &stop, user);

        if (!ok) { rb_abort(rb); break; }
        if (len == 0u && stop) { rb_abort(rb); break; }
        if (len > cap) { len = cap; truncated = true; }

        rb_publish_ex(rb, idx, len, truncated);
        n++;
        if (stop) break;
    }
    return n;
}

/* ---------------- introspection ---------------- */

uint32_t rb_count(const rb_t *rb) {
    if (!rb || !rb->scratch) return 0u;
    uint32_t h = RB_ATOMIC_LOAD_ACQ(&rb->head);
    uint32_t t = RB_ATOMIC_LOAD_ACQ(&rb->tail);
    return h - t;
}

uint32_t rb_capacity(const rb_t *rb) { return rb ? rb->capacity : 0u; }
uint32_t rb_limit(const rb_t *rb)    { return rb ? rb->limit    : 0u; }

bool rb_is_full(const rb_t *rb)  { return rb_count(rb) >= rb_limit(rb); }
bool rb_is_empty(const rb_t *rb) { return rb_count(rb) == 0u; }

void *rb_slot_ptr(rb_t *rb, uint32_t slot_index) {
    if (!rb || !rb->scratch || slot_index >= rb->slots) return NULL;
    return rb->scratch + (size_t)slot_index * rb->slot_stride + RB_SLOT_HDR_SIZE;
}

const void *rb_slot_cptr(const rb_t *rb, uint32_t slot_index) {
    if (!rb || !rb->scratch || slot_index >= rb->slots) return NULL;
    return rb->scratch + (size_t)slot_index * rb->slot_stride + RB_SLOT_HDR_SIZE;
}

#if RB_ENABLE_STATS
const rb_stats_t *rb_stats(const rb_t *rb) { return rb ? &rb->stats : NULL; }

void rb_stats_reset(rb_t *rb) {
    if (rb) memset(&rb->stats, 0, sizeof rb->stats);
}
#endif

/* ---------------- runtime reconfig ---------------- */

rb_err_t rb_set_limit(rb_t *rb, uint32_t limit) {
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (limit == 0u || limit > rb->capacity || limit > rb->slots)
        return RB_ERR_INVAL;
    if (rb_count(rb) != 0u) return RB_ERR_INVAL;
    rb->limit = limit;
    return RB_OK;
}

rb_err_t rb_set_low_d(rb_t *rb, uint32_t percent) {
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (percent > 100u) return RB_ERR_INVAL;
    rb->low_d = percent;
    return RB_OK;
}

rb_err_t rb_set_low_e(rb_t *rb, uint32_t percent) {
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (percent > 100u) return RB_ERR_INVAL;
    rb->low_e = percent;
    return RB_OK;
}

rb_err_t rb_set_oversize_policy(rb_t *rb, rb_oversize_policy_t p) {
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    if (p != RB_OVERSIZE_TRUNCATE && p != RB_OVERSIZE_DROP)
        return RB_ERR_INVAL;
    rb->oversize_policy = p;
    return RB_OK;
}

rb_err_t rb_set_callbacks(rb_t *rb, const rb_callbacks_t *cb) {
    if (!rb || !rb->scratch || !cb) return RB_ERR_INVAL;
    rb->cb = *cb;
    return RB_OK;
}

rb_err_t rb_set_producer_stack(rb_t *rb, size_t bytes) {
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    rb->producer_stack_size = bytes ? bytes : RB_DEFAULT_PRODUCER_STACK;
    return RB_OK;
}

rb_err_t rb_set_consumer_stack(rb_t *rb, size_t bytes) {
    if (!rb || !rb->scratch) return RB_ERR_NOT_INIT;
    rb->consumer_stack_size = bytes ? bytes : RB_DEFAULT_CONSUMER_STACK;
    return RB_OK;
}
