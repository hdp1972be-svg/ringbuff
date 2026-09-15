#define _GNU_SOURCE
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */
#include "rb.h"
#include "rb_port.h"
#include <string.h>
#define RB_NO_PENDING 0xFFFFFFFFu
#if RB_ENABLE_NOTIFY && defined(__linux__)
#include <errno.h>
#include <linux/futex.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static int rb_futex_wait(uint32_t *word, uint32_t expected, const struct timespec *ts) {
#if RB_FUTEX_SHARED
    return (int)syscall(SYS_futex, word, FUTEX_WAIT, expected, ts, NULL, 0);
#else
    return (int)syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expected, ts, NULL, 0);
#endif
}
static int rb_futex_wake(uint32_t *word) {
#if RB_FUTEX_SHARED
    return (int)syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0);
#else
    return (int)syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#endif
}
#endif
struct rb_s {
#if RB_PER_SLOT_LAP
    RB_ALIGNAS(RB_CACHE_LINE) rb_atomic_u32 notify_seq;
    uint32_t cached_consumer_pos, pending_slot, pending_wanted;
#else
    RB_ALIGNAS(RB_CACHE_LINE) rb_atomic_u32 head;
    uint32_t cached_tail, pending_slot, pending_wanted;
#endif
    rb_atomic_u32 full_latch;
#if RB_ENABLE_NOTIFY && defined(__linux__)
    rb_atomic_u32 notify_waiters;
    int notify_fd;
#endif
    #if RB_PER_SLOT_LAP
    RB_ALIGNAS(RB_CACHE_LINE) rb_atomic_u32 consumer_pos;
    uint32_t cached_notify_seq, consumer_active, consumer_slot, low_d_latch, low_e_latch;
#else
    RB_ALIGNAS(RB_CACHE_LINE) rb_atomic_u32 tail;
    uint32_t cached_head, consumer_active, consumer_slot, low_d_latch, low_e_latch;
#endif
    uint32_t capacity, limit, mask, slots, slots_mask, slot_size, slot_stride, low_d, low_e;
    rb_oversize_policy_t oversize_policy;
    size_t producer_stack_size, consumer_stack_size;
    size_t scratch_off;
    size_t scratch_size;
    rb_callbacks_t cb;
#if RB_ENABLE_STATS
    rb_stats_t stats;
#endif
    rb_entry_t entries[];
};
static inline uint32_t slot_stride_for(uint32_t slot_size) {
#if RB_SLOT_CACHELINE_PAD
    size_t a = RB_CACHE_LINE;
#else
    size_t a = _Alignof(max_align_t);
#endif
    if (a < 4)
        a = 4;
    return (uint32_t)RB_ALIGN_UP(slot_size, a);
}
static inline bool below_low(uint32_t count, uint32_t limit, uint32_t percent) {
    return percent && (uint64_t)count * 100u < (uint64_t)limit * percent;
}
static inline uint32_t slot_index_for(const rb_t *rb, uint32_t head) {
    return rb->slots_mask ? (head & rb->slots_mask) : (head % rb->slots);
}
static inline uint8_t *rb_scratch(rb_t *rb) {
    return (uint8_t *)rb + rb->scratch_off;
}
static inline const uint8_t *rb_scratch_c(const rb_t *rb) {
    return (const uint8_t *)rb + rb->scratch_off;
}
void rb_config_init(rb_config_t *cfg) {
    if (!cfg)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->capacity = RB_CAPACITY;
    cfg->slots = RB_NUM_SLOTS;
    cfg->slot_size = RB_SLOT_SIZE;
    cfg->low_d = RB_DEFAULT_LOW_D;
    cfg->low_e = RB_DEFAULT_LOW_E;
    cfg->oversize_policy = RB_OVERSIZE_TRUNCATE;
}
void rb_config_set_capacity(rb_config_t *c, uint32_t v) {
    if (c)
        c->capacity = v;
}
void rb_config_set_limit(rb_config_t *c, uint32_t v) {
    if (c)
        c->limit = v;
}
void rb_config_set_slots(rb_config_t *c, uint32_t v) {
    if (c)
        c->slots = v;
}
void rb_config_set_slot_size(rb_config_t *c, uint32_t v) {
    if (c)
        c->slot_size = v;
}
void rb_config_set_low_d(rb_config_t *c, uint32_t v) {
    if (c)
        c->low_d = v;
}
void rb_config_set_low_e(rb_config_t *c, uint32_t v) {
    if (c)
        c->low_e = v;
}
void rb_config_set_oversize_policy(rb_config_t *c, rb_oversize_policy_t p) {
    if (c)
        c->oversize_policy = p;
}
void rb_config_set_callbacks(rb_config_t *c, const rb_callbacks_t *cb) {
    if (c && cb)
        c->cb = *cb;
}
void rb_config_set_producer_stack(rb_config_t *c, size_t v) {
    if (c)
        c->producer_stack_size = v;
}
void rb_config_set_consumer_stack(rb_config_t *c, size_t v) {
    if (c)
        c->consumer_stack_size = v;
}
size_t rb_size(uint32_t capacity) {
    return offsetof(rb_t, entries) + (size_t)capacity * sizeof(rb_entry_t);
}
size_t rb_producer_stack(const rb_t *rb) {
    return rb ? rb->producer_stack_size : 0u;
}
size_t rb_consumer_stack(const rb_t *rb) {
    return rb ? rb->consumer_stack_size : 0u;
}
rb_err_t rb_init(rb_t *rb, const rb_config_t *cfg, void *scratch, size_t scratch_size) {
    if (!rb || !cfg || !scratch)
        return RB_ERR_INVAL;
    if (((uintptr_t)rb % _Alignof(rb_t)) != 0)
        return RB_ERR_INVAL;
    uint32_t capacity = cfg->capacity ? cfg->capacity : RB_CAPACITY;
    if (!RB_IS_POW2(capacity) || capacity < 2u)
        return RB_ERR_INVAL;
    uint32_t slots = cfg->slots ? cfg->slots : RB_NUM_SLOTS;
    if (!slots)
        return RB_ERR_INVAL;
    uint32_t slot_size = cfg->slot_size ? cfg->slot_size : RB_SLOT_SIZE;
    if (slot_size <= RB_SLOT_HDR_SIZE)
        return RB_ERR_INVAL;
    uint32_t limit = cfg->limit ? cfg->limit : capacity;
    if (limit > capacity)
        limit = capacity;
    if (limit > slots)
        limit = slots;
    if (!limit || cfg->low_d > 100u || cfg->low_e > 100u || cfg->low_e > cfg->low_d)
        return RB_ERR_INVAL;
#if RB_SLOT_CACHELINE_PAD
    size_t need_align = RB_CACHE_LINE;
#else
    size_t need_align = _Alignof(max_align_t);
#endif
    if (need_align < 4u)
        need_align = 4u;
    if (((uintptr_t)scratch % need_align) != 0u)
        return RB_ERR_INVAL;
    uint32_t stride = slot_stride_for(slot_size);
    size_t need = (size_t)slots * stride;
    if (scratch_size < need)
        return RB_ERR_INVAL;
    memset(rb, 0, offsetof(rb_t, entries));
    rb->capacity = capacity;
    rb->limit = limit;
    rb->mask = capacity - 1u;
    rb->slots = slots;
    rb->slots_mask = RB_IS_POW2(slots) ? slots - 1u : 0u;
    rb->slot_size = slot_size;
    rb->slot_stride = stride;
    rb->low_d = cfg->low_d;
    rb->low_e = cfg->low_e;
    rb->oversize_policy = cfg->oversize_policy;
    rb->producer_stack_size =
        cfg->producer_stack_size ? cfg->producer_stack_size : RB_DEFAULT_PRODUCER_STACK;
    rb->consumer_stack_size =
        cfg->consumer_stack_size ? cfg->consumer_stack_size : RB_DEFAULT_CONSUMER_STACK;
    rb->scratch_off = (size_t)((uintptr_t)scratch - (uintptr_t)rb);
    rb->scratch_size = scratch_size;
    rb->cb = cfg->cb;
    #if RB_PER_SLOT_LAP
    RB_ATOMIC_STORE_REL(&rb->notify_seq, 0u);
    RB_ATOMIC_STORE_REL(&rb->consumer_pos, 0u);
    rb->cached_consumer_pos = rb->cached_notify_seq = 0u;
#else
    RB_ATOMIC_STORE_REL(&rb->head, 0u);
    RB_ATOMIC_STORE_REL(&rb->tail, 0u);
    rb->cached_tail = rb->cached_head = 0u;
#endif
    rb->pending_slot = RB_NO_PENDING;
    rb->pending_wanted = 0u;
    rb->consumer_active = rb->consumer_slot = 0u;
    RB_ATOMIC_STORE_RLX(&rb->full_latch, 0u);
    rb->low_d_latch = rb->low_e_latch = 0u;
#if RB_PER_SLOT_LAP
    {
        uint32_t si = 0u;
        for (; si < slots; si++)
            RB_ATOMIC_STORE_RLX((rb_atomic_u32 *)(rb_scratch(rb) + (size_t)si * stride), si);
    }
#endif
#if RB_ENABLE_NOTIFY && defined(__linux__)
    RB_ATOMIC_STORE_RLX(&rb->notify_waiters, 0u);
    rb->notify_fd = -1;
#endif
#if RB_ENABLE_STATS
    memset(&rb->stats, 0, sizeof rb->stats);
#endif
    return RB_OK;
}
void rb_deinit(rb_t *rb) {
    if (!rb)
        return;
#if RB_ENABLE_NOTIFY && defined(__linux__)
    if (rb->notify_fd >= 0) {
        close(rb->notify_fd);
        rb->notify_fd = -1;
    }
#endif
    rb->scratch_off = 0;
    rb->scratch_size = 0;
    rb->pending_slot = RB_NO_PENDING;
    rb->consumer_active = 0u;
}
rb_err_t rb_acquire(rb_t *rb, uint32_t wanted_len, uint32_t *out_slot_index, void **out_writable,
                    uint32_t *out_cap) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (rb->pending_slot != RB_NO_PENDING)
        return RB_ERR_INVAL;
    #if RB_PER_SLOT_LAP
    uint32_t pos = RB_ATOMIC_LOAD_RLX(&rb->notify_seq), tail = rb->cached_consumer_pos, count = pos - tail;
    if (count >= rb->limit) {
        tail = RB_ATOMIC_LOAD_ACQ(&rb->consumer_pos);
        rb->cached_consumer_pos = tail;
        count = pos - tail;
        if (count >= rb->limit) {
#else
    uint32_t head = RB_ATOMIC_LOAD_RLX(&rb->head), tail = rb->cached_tail, count = head - tail;
    if (count >= rb->limit) {
        tail = RB_ATOMIC_LOAD_ACQ(&rb->tail);
        rb->cached_tail = tail;
        count = head - tail;
        if (count >= rb->limit) {
#endif
#if RB_ENABLE_STATS
            rb->stats.full_attempts++;
#endif
            return RB_ERR_FULL;
        }
    }
    uint32_t cap = rb->slot_size - RB_SLOT_HDR_SIZE;
    if (wanted_len && wanted_len > cap && rb->oversize_policy == RB_OVERSIZE_DROP)
        return RB_ERR_OVERSIZE;
    #if RB_PER_SLOT_LAP
    uint32_t slot_index = slot_index_for(rb, pos);
#else
    uint32_t slot_index = slot_index_for(rb, head);
#endif
    rb->pending_slot = slot_index;
    rb->pending_wanted = wanted_len;
    if (out_slot_index)
        *out_slot_index = slot_index;
    if (out_writable) {
        uint8_t *slot = rb_scratch(rb) + (size_t)slot_index * rb->slot_stride;
        void *w = slot + RB_SLOT_HDR_SIZE;
        *out_writable = w;
        /* The caller will write into this line next. Start the fetch now,
           while it does whatever prep work precedes its memcpy. */
        RB_PREFETCH_W(w);
    }
    if (out_cap)
        *out_cap = cap;
    return RB_OK;
}
rb_err_t rb_publish_ex(rb_t *rb, uint32_t slot_index, uint32_t written_len, bool truncated) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (rb->pending_slot == RB_NO_PENDING || rb->pending_slot != slot_index)
        return RB_ERR_INVAL;
    uint32_t cap = rb->slot_size - RB_SLOT_HDR_SIZE;
    if (written_len > cap) {
        written_len = cap;
        truncated = true;
    }
    if (rb->pending_wanted > cap)
        truncated = true;
    uint32_t hdr = written_len & RB_SLOT_LEN_MASK;
    if (truncated)
        hdr |= RB_SLOT_TRUNCATED;
    uint8_t *slot = rb_scratch(rb) + (size_t)slot_index * rb->slot_stride;
    memcpy(slot + (RB_SLOT_HDR_SIZE - sizeof hdr), &hdr, sizeof hdr);
#if RB_PER_SLOT_LAP
    uint32_t pos = RB_ATOMIC_LOAD_RLX(&rb->notify_seq);
    RB_ATOMIC_STORE_REL((rb_atomic_u32 *)slot, pos + 1u);
    RB_ATOMIC_STORE_REL(&rb->notify_seq, pos + 1u);
#else
    uint32_t head = RB_ATOMIC_LOAD_RLX(&rb->head);
#if RB_USE_POINTERS
    rb->entries[head & rb->mask] = (rb_entry_t)slot;
#else
    rb->entries[head & rb->mask] = (rb_entry_t)slot_index;
#endif
    RB_ATOMIC_STORE_REL(&rb->head, head + 1u);
#endif
#if RB_ENABLE_NOTIFY && defined(__linux__)
    if (RB_ATOMIC_LOAD_RLX(&rb->notify_waiters) != 0u)
#if RB_PER_SLOT_LAP
        (void)rb_futex_wake((uint32_t *)&rb->notify_seq);
#else
        (void)rb_futex_wake((uint32_t *)&rb->head);
#endif
    if (rb->notify_fd >= 0) {
        uint64_t one = 1u;
        ssize_t _w = write(rb->notify_fd, &one, sizeof one);
        (void)_w;
    }
#endif
    rb->pending_slot = RB_NO_PENDING;
    rb->pending_wanted = 0u;
#if RB_PER_SLOT_LAP
    uint32_t tail = RB_ATOMIC_LOAD_ACQ(&rb->consumer_pos);
    rb->cached_consumer_pos = tail;
    uint32_t count = (pos + 1u) - tail;
#else
    uint32_t tail = RB_ATOMIC_LOAD_ACQ(&rb->tail);
    rb->cached_tail = tail;
    uint32_t count = (head + 1u) - tail;
#endif
#if RB_ENABLE_STATS
    rb->stats.published++;
    if (count > rb->stats.high_water)
        rb->stats.high_water = count;
    if (truncated)
        rb->stats.truncated++;
#endif
    if (rb->cb.on_slot_added)
        rb->cb.on_slot_added(rb, slot_index, written_len, truncated, rb->cb.user);
    if (count >= rb->limit && RB_ATOMIC_LOAD_RLX(&rb->full_latch) == 0u) {
        RB_ATOMIC_STORE_RLX(&rb->full_latch, 1u);
#if RB_ENABLE_STATS
        rb->stats.full_hits++;
#endif
        if (rb->cb.on_full)
            rb->cb.on_full(rb, rb->cb.user);
    }
    return RB_OK;
}
rb_err_t rb_publish(rb_t *rb, uint32_t slot_index, uint32_t written_len) {
    return rb_publish_ex(rb, slot_index, written_len, false);
}
rb_err_t rb_abort(rb_t *rb) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (rb->pending_slot == RB_NO_PENDING)
        return RB_ERR_INVAL;
    rb->pending_slot = RB_NO_PENDING;
    rb->pending_wanted = 0u;
    return RB_OK;
}
rb_err_t rb_consume(rb_t *rb, uint32_t *out_slot_index, const void **out_obj, uint32_t *out_len,
                    bool *out_truncated) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (rb->consumer_active)
        return RB_ERR_INVAL;
    #if RB_PER_SLOT_LAP
    uint32_t tail = RB_ATOMIC_LOAD_RLX(&rb->consumer_pos), head = rb->cached_notify_seq;
    if (tail == head) {
        head = RB_ATOMIC_LOAD_ACQ(&rb->notify_seq);
        rb->cached_notify_seq = head;
        if (tail == head)
            return RB_ERR_EMPTY;
    }
    uint32_t slot_index = slot_index_for(rb, tail);
    if (slot_index >= rb->slots)
        return RB_ERR_INVAL;
    const uint8_t *slot = rb_scratch(rb) + (size_t)slot_index * rb->slot_stride;
    if (RB_ATOMIC_LOAD_ACQ((rb_atomic_u32 *)slot) != tail + 1u) {
        head = RB_ATOMIC_LOAD_ACQ(&rb->notify_seq);
        rb->cached_notify_seq = head;
        if (head == tail)
            return RB_ERR_EMPTY;
    }
#else
    uint32_t tail = RB_ATOMIC_LOAD_RLX(&rb->tail), head = rb->cached_head;
    if (tail == head) {
        head = RB_ATOMIC_LOAD_ACQ(&rb->head);
        rb->cached_head = head;
        if (tail == head)
            return RB_ERR_EMPTY;
    }
    rb_entry_t entry = rb->entries[tail & rb->mask];
    uint32_t slot_index;
#if RB_USE_POINTERS
    if ((const uint8_t *)entry < rb_scratch(rb))
        return RB_ERR_INVAL;
    uintptr_t off = (uintptr_t)((const uint8_t *)entry - rb_scratch(rb));
    if (off % rb->slot_stride)
        return RB_ERR_INVAL;
    slot_index = (uint32_t)(off / rb->slot_stride);
#else
    slot_index = (uint32_t)entry;
#endif
    if (slot_index >= rb->slots)
        return RB_ERR_INVAL;
    const uint8_t *slot = rb_scratch(rb) + (size_t)slot_index * rb->slot_stride;
#endif
    uint32_t hdr;
    memcpy(&hdr, slot + (RB_SLOT_HDR_SIZE - sizeof hdr), sizeof hdr);
    uint32_t len = hdr & RB_SLOT_LEN_MASK;
    bool truncated = (hdr & RB_SLOT_TRUNCATED) != 0u;
    rb->consumer_active = 1u;
    rb->consumer_slot = slot_index;
    if (out_slot_index)
        *out_slot_index = slot_index;
    if (out_obj) {
        const void *obj = slot + RB_SLOT_HDR_SIZE;
        *out_obj = obj;
        /* The caller will read from this line next. */
        RB_PREFETCH_R(obj);
    }
    if (out_len)
        *out_len = len;
    if (out_truncated)
        *out_truncated = truncated;
    return RB_OK;
}
rb_err_t rb_release(rb_t *rb, uint32_t slot_index) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (!rb->consumer_active || slot_index != rb->consumer_slot)
        return RB_ERR_INVAL;
    #if RB_PER_SLOT_LAP
    uint32_t tail = RB_ATOMIC_LOAD_RLX(&rb->consumer_pos);
    uint8_t *slot = rb_scratch(rb) + (size_t)slot_index * rb->slot_stride;
    RB_ATOMIC_STORE_REL((rb_atomic_u32 *)slot, tail + rb->slots);
    RB_ATOMIC_STORE_REL(&rb->consumer_pos, tail + 1u);
    rb->consumer_active = 0u;
    uint32_t head = rb->cached_notify_seq, new_count = head - (tail + 1u), old_count = new_count + 1u;
#else
    uint32_t tail = RB_ATOMIC_LOAD_RLX(&rb->tail);
    RB_ATOMIC_STORE_REL(&rb->tail, tail + 1u);
    rb->consumer_active = 0u;
    uint32_t head = rb->cached_head, new_count = head - (tail + 1u), old_count = new_count + 1u;
#endif
#if RB_ENABLE_STATS
    rb->stats.consumed++;
#endif
    bool old_d = below_low(old_count, rb->limit, rb->low_d),
         new_d = below_low(new_count, rb->limit, rb->low_d);
    if (!old_d && new_d) {
        rb->low_d_latch = 1u;
        RB_ATOMIC_STORE_RLX(&rb->full_latch, 0u);
#if RB_ENABLE_STATS
        rb->stats.low_d_hits++;
#endif
        if (rb->cb.on_low_d)
            rb->cb.on_low_d(rb, rb->cb.user);
    } else if (old_d && !new_d)
        RB_ATOMIC_STORE_RLX(&rb->full_latch, 0u);
    bool old_e = below_low(old_count, rb->limit, rb->low_e),
         new_e = below_low(new_count, rb->limit, rb->low_e);
    if (!old_e && new_e) {
        rb->low_e_latch = 1u;
#if RB_ENABLE_STATS
        rb->stats.low_e_hits++;
#endif
        if (rb->cb.on_low_e)
            rb->cb.on_low_e(rb, rb->cb.user);
    } else if (old_e && !new_e)
        rb->low_e_latch = 0u;
    return RB_OK;
}
uint32_t rb_drain(rb_t *rb, rb_drain_fn fn, void *user) {
    if (!rb || !rb->scratch_size || !fn)
        return 0u;
    uint32_t n = 0u;
    for (;;) {
        uint32_t idx = 0u, len = 0u;
        const void *obj = NULL;
        bool truncated = false;
        if (rb_consume(rb, &idx, &obj, &len, &truncated) != RB_OK)
            break;
        bool cont = fn(obj, len, truncated, user);
        rb_release(rb, idx);
        n++;
        if (!cont)
            break;
    }
    return n;
}
uint32_t rb_flush(rb_t *rb, rb_flush_fn fn, void *user) {
    if (!rb || !rb->scratch_size || !fn)
        return 0u;
    uint32_t n = 0u;
    for (;;) {
        uint32_t idx = 0u, cap = 0u;
        void *w = NULL;
        if (rb_acquire(rb, 0u, &idx, &w, &cap) != RB_OK)
            break;
        uint32_t len = 0u;
        bool truncated = false, stop = false;
        bool ok = fn(w, cap, &len, &truncated, &stop, user);
        if (!ok) {
            rb_abort(rb);
            break;
        }
        if (len == 0u && stop) {
            rb_abort(rb);
            break;
        }
        if (len > cap) {
            len = cap;
            truncated = true;
        }
        rb_publish_ex(rb, idx, len, truncated);
        n++;
        if (stop)
            break;
    }
    return n;
}
uint32_t rb_count(const rb_t *rb) {
    if (!rb || !rb->scratch_size)
        return 0u;
    #if RB_PER_SLOT_LAP
    return RB_ATOMIC_LOAD_ACQ(&rb->notify_seq) - RB_ATOMIC_LOAD_ACQ(&rb->consumer_pos);
#else
    return RB_ATOMIC_LOAD_ACQ(&rb->head) - RB_ATOMIC_LOAD_ACQ(&rb->tail);
#endif
}
uint32_t rb_capacity(const rb_t *rb) {
    return rb ? rb->capacity : 0u;
}
uint32_t rb_limit(const rb_t *rb) {
    return rb ? rb->limit : 0u;
}
bool rb_is_full(const rb_t *rb) {
    return rb_count(rb) >= rb_limit(rb);
}
bool rb_is_empty(const rb_t *rb) {
    return rb_count(rb) == 0u;
}
void *rb_slot_ptr(rb_t *rb, uint32_t slot_index) {
    if (!rb || !rb->scratch_size || slot_index >= rb->slots)
        return NULL;
    return rb_scratch(rb) + (size_t)slot_index * rb->slot_stride + RB_SLOT_HDR_SIZE;
}
const void *rb_slot_cptr(const rb_t *rb, uint32_t slot_index) {
    if (!rb || !rb->scratch_size || slot_index >= rb->slots)
        return NULL;
    return rb_scratch_c(rb) + (size_t)slot_index * rb->slot_stride + RB_SLOT_HDR_SIZE;
}
#if RB_ENABLE_STATS
const rb_stats_t *rb_stats(const rb_t *rb) {
    return rb ? &rb->stats : NULL;
}
void rb_stats_reset(rb_t *rb) {
    if (rb)
        memset(&rb->stats, 0, sizeof rb->stats);
}
#endif
rb_err_t rb_set_limit(rb_t *rb, uint32_t limit) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (!limit || limit > rb->capacity || limit > rb->slots || rb_count(rb) != 0u)
        return RB_ERR_INVAL;
    rb->limit = limit;
    return RB_OK;
}
rb_err_t rb_set_low_d(rb_t *rb, uint32_t percent) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (percent > 100u)
        return RB_ERR_INVAL;
    rb->low_d = percent;
    return RB_OK;
}
rb_err_t rb_set_low_e(rb_t *rb, uint32_t percent) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (percent > 100u)
        return RB_ERR_INVAL;
    rb->low_e = percent;
    return RB_OK;
}
rb_err_t rb_set_oversize_policy(rb_t *rb, rb_oversize_policy_t p) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    if (p != RB_OVERSIZE_TRUNCATE && p != RB_OVERSIZE_DROP)
        return RB_ERR_INVAL;
    rb->oversize_policy = p;
    return RB_OK;
}
rb_err_t rb_set_callbacks(rb_t *rb, const rb_callbacks_t *cb) {
    if (!rb || !rb->scratch_size || !cb)
        return RB_ERR_INVAL;
    rb->cb = *cb;
    return RB_OK;
}
rb_err_t rb_set_producer_stack(rb_t *rb, size_t bytes) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    rb->producer_stack_size = bytes ? bytes : RB_DEFAULT_PRODUCER_STACK;
    return RB_OK;
}
rb_err_t rb_set_consumer_stack(rb_t *rb, size_t bytes) {
    if (!rb || !rb->scratch_size)
        return RB_ERR_NOT_INIT;
    rb->consumer_stack_size = bytes ? bytes : RB_DEFAULT_CONSUMER_STACK;
    return RB_OK;
}
#if RB_ENABLE_NOTIFY && defined(__linux__)
uint32_t rb_notify_value(const rb_t *rb) {
#if RB_PER_SLOT_LAP
    return rb ? RB_ATOMIC_LOAD_ACQ(&rb->notify_seq) : 0u;
#else
    return rb ? RB_ATOMIC_LOAD_ACQ(&rb->head) : 0u;
#endif
}
static uint64_t rb_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}
int rb_wait(rb_t *rb, uint32_t expected, int timeout_ms) {
    if (!rb || !rb->scratch_size)
        return -EINVAL;
    if (rb_count(rb) != 0u)
        return 0;
    const uint64_t start = rb_now_ms();
    const uint64_t deadline = timeout_ms >= 0 ? start + (uint64_t)timeout_ms : UINT64_MAX;
    RB_ATOMIC_FETCH_ADD(&rb->notify_waiters, 1u);
    int result = 0;
    for (;;) {
        if (rb_count(rb) != 0u)
            break;
        uint64_t now = rb_now_ms();
        if (timeout_ms >= 0 && now >= deadline) {
            result = -ETIMEDOUT;
            break;
        }
        uint64_t remain = timeout_ms >= 0 ? deadline - now : (uint64_t)RB_NOTIFY_WAIT_SLICE_MS;
        uint64_t slice =
            remain < (uint64_t)RB_NOTIFY_WAIT_SLICE_MS ? remain : (uint64_t)RB_NOTIFY_WAIT_SLICE_MS;
        if (slice == 0u) {
            result = -ETIMEDOUT;
            break;
        }
        struct timespec ts = {(time_t)(slice / 1000u), (long)((slice % 1000u) * 1000000u)};
        #if RB_PER_SLOT_LAP
        int rc = rb_futex_wait((uint32_t *)&rb->notify_seq, expected, &ts);
#else
        int rc = rb_futex_wait((uint32_t *)&rb->head, expected, &ts);
#endif
        int saved = errno;
        if (rc == 0 || saved == EAGAIN)
            break;
        if (saved == EINTR)
            continue;
        if (saved == ETIMEDOUT) {
            if (timeout_ms < 0)
                continue;
            if (rb_now_ms() >= deadline) {
                result = -ETIMEDOUT;
                break;
            }
            continue;
        }
        result = -saved;
        break;
    }
    RB_ATOMIC_FETCH_SUB(&rb->notify_waiters, 1u);
    return result;
}
int rb_notify_fd(rb_t *rb) {
    if (!rb || !rb->scratch_size)
        return -EINVAL;
    if (rb->notify_fd >= 0)
        return rb->notify_fd;
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
        return -errno;
    rb->notify_fd = fd;
    return fd;
}
int rb_notify_drain_fd(rb_t *rb) {
    if (!rb || rb->notify_fd < 0)
        return -EINVAL;
    uint64_t value;
    for (;;) {
        ssize_t n = read(rb->notify_fd, &value, sizeof value);
        if (n == (ssize_t)sizeof value)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n < 0)
            return -errno;
        return -EIO;
    }
}
#endif
