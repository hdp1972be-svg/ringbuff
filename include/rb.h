/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

#ifndef RB_H
#define RB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "rb_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RB_VERSION_MAJOR 1
#define RB_VERSION_MINOR 0
#define RB_VERSION_PATCH 0
#define RB_VERSION_STRING "1.0.0"

/* ---------------- Errors ---------------- */
typedef enum {
    RB_OK = 0,
    RB_ERR_FULL,
    RB_ERR_EMPTY,
    RB_ERR_OVERSIZE,
    RB_ERR_INVAL,
    RB_ERR_NOT_INIT
} rb_err_t;

/* ---------------- Slot header ----------------
 * 4 bytes at the start of each slot.
 * Top bit = TRUNCATED, low 31 bits = payload bytes actually written.
 */
#define RB_SLOT_TRUNCATED 0x80000000u
#define RB_SLOT_LEN_MASK  0x7FFFFFFFu
#define RB_SLOT_HDR_SIZE  4u

/* ---------------- Oversize policy ---------------- */
typedef enum {
    RB_OVERSIZE_TRUNCATE = 0,  /* write cap bytes, flag TRUNCATED, publish */
    RB_OVERSIZE_DROP     = 1   /* return RB_ERR_OVERSIZE, do not publish */
} rb_oversize_policy_t;

/* ---------------- Entry representation ---------------- */
#if RB_USE_POINTERS
typedef void *rb_entry_t;
#else
typedef uint32_t rb_entry_t;
#endif

/* ---------------- Opaque handle ---------------- */
typedef struct rb_s rb_t;

/* ---------------- Callbacks ----------------
 * All informational. Never gate control flow, never block, never
 * re-enter the queue.
 */
typedef void (*rb_cb_t)(rb_t *rb, void *user);
typedef void (*rb_slot_cb_t)(rb_t *rb, uint32_t slot_index,
                             uint32_t len, bool truncated, void *user);

typedef struct {
    rb_slot_cb_t on_slot_added;  /* every successful publish */
    rb_cb_t      on_low_d;       /* count crossed below d% */
    rb_cb_t      on_low_e;       /* count crossed below e% */
    rb_cb_t      on_full;        /* count reached limit */
    void        *user;
} rb_callbacks_t;

/* ---------------- Stats ---------------- */
#if RB_ENABLE_STATS
typedef struct {
    uint64_t published;
    uint64_t consumed;
    uint64_t truncated;
    uint64_t full_attempts;   /* rb_acquire returned RB_ERR_FULL */
    uint32_t high_water;
    uint32_t full_hits;       /* on_full fired */
    uint32_t low_d_hits;
    uint32_t low_e_hits;
} rb_stats_t;
#endif

/* ---------------- Config ---------------- */
typedef struct {
    uint32_t capacity;      /* power of two; 0 -> RB_CAPACITY */
    uint32_t limit;         /* logical max entries; 0 -> capacity */
    uint32_t slots;         /* z; 0 -> RB_NUM_SLOTS */
    uint32_t slot_size;     /* y incl. header; 0 -> RB_SLOT_SIZE */
    uint32_t low_d;         /* percent 0..100 */
    uint32_t low_e;         /* percent 0..100, must be <= low_d */
    rb_oversize_policy_t oversize_policy;
    size_t   producer_stack_size;
    size_t   consumer_stack_size;
    rb_callbacks_t cb;
} rb_config_t;

void rb_config_init(rb_config_t *cfg);
void rb_config_set_capacity(rb_config_t *cfg, uint32_t capacity);
void rb_config_set_limit(rb_config_t *cfg, uint32_t limit);
void rb_config_set_slots(rb_config_t *cfg, uint32_t slots);
void rb_config_set_slot_size(rb_config_t *cfg, uint32_t slot_size);
void rb_config_set_low_d(rb_config_t *cfg, uint32_t percent);
void rb_config_set_low_e(rb_config_t *cfg, uint32_t percent);
void rb_config_set_oversize_policy(rb_config_t *cfg, rb_oversize_policy_t p);
void rb_config_set_callbacks(rb_config_t *cfg, const rb_callbacks_t *cb);
void rb_config_set_producer_stack(rb_config_t *cfg, size_t bytes);
void rb_config_set_consumer_stack(rb_config_t *cfg, size_t bytes);

/* ---------------- Lifecycle ----------------
 * rb_size(capacity) = bytes needed for the control block.
 *   The returned block must be aligned to at least _Alignof(rb_t)
 *   (== RB_CACHE_LINE in practice). Use posix_memalign or _Alignas.
 * rb_init installs a caller-owned scratchpad of at least
 *   slots * align_up(slot_size, RB_SLOT_CACHELINE_PAD ? RB_CACHE_LINE
 *                                                     : _Alignof(max_align_t))
 * bytes, aligned the same way.
 */
size_t    rb_size(uint32_t capacity);
rb_err_t  rb_init(rb_t *rb, const rb_config_t *cfg,
                  void *scratch, size_t scratch_size);
void      rb_deinit(rb_t *rb);
size_t rb_producer_stack(const rb_t *rb);
size_t rb_consumer_stack(const rb_t *rb);

/* ---------------- Producer ----------------
 * rb_acquire returns a writable region past the slot header.
 *   wanted_len = caller's intended payload size.
 *     Under RB_OVERSIZE_DROP, wanted_len > cap returns RB_ERR_OVERSIZE.
 *     Under RB_OVERSIZE_TRUNCATE, wanted_len > cap is noted and the
 *       TRUNCATED flag is set at publish.
 *   wanted_len = 0 means "unknown / no pre-check" (used by rb_flush).
 */
rb_err_t  rb_acquire(rb_t *rb, uint32_t wanted_len,
                     uint32_t *out_slot_index,
                     void **out_writable, uint32_t *out_cap);

rb_err_t  rb_publish(rb_t *rb, uint32_t slot_index, uint32_t written_len);

/* Explicit-truncation variant; rb_publish is a wrapper around this. */
rb_err_t  rb_publish_ex(rb_t *rb, uint32_t slot_index,
                        uint32_t written_len, bool truncated);

/* Cancel an in-flight acquire without publishing. */
rb_err_t  rb_abort(rb_t *rb);

/* ---------------- Consumer ----------------
 * rb_consume gives a read-only view past the header.
 * Must be paired with rb_release before the next rb_consume.
 * Release order is strict FIFO.
 */
rb_err_t  rb_consume(rb_t *rb, uint32_t *out_slot_index,
                     const void **out_obj, uint32_t *out_len,
                     bool *out_truncated);
rb_err_t  rb_release(rb_t *rb, uint32_t slot_index);

/* ---------------- Drain / Flush ---------------- */
typedef bool (*rb_drain_fn)(const void *obj, uint32_t len,
                            bool truncated, void *user);
/* return true to continue, false to stop after this slot */

uint32_t rb_drain(rb_t *rb, rb_drain_fn fn, void *user);
/* drains until empty; returns slots processed */

typedef bool (*rb_flush_fn)(void *writable, uint32_t cap,
                            uint32_t *out_len, bool *out_truncated,
                            bool *out_stop, void *user);
/* fill writable, set *out_len. Set *out_truncated if more was intended
   than `cap`. Set *out_stop = true to end the flush after this slot.
   Return false to abort without publishing. */

uint32_t rb_flush(rb_t *rb, rb_flush_fn fn, void *user);

/* ---------------- Introspection ---------------- */
uint32_t rb_count(const rb_t *rb);
uint32_t rb_capacity(const rb_t *rb);
uint32_t rb_limit(const rb_t *rb);
bool     rb_is_full(const rb_t *rb);
bool     rb_is_empty(const rb_t *rb);

void    *rb_slot_ptr(rb_t *rb, uint32_t slot_index);
const void *rb_slot_cptr(const rb_t *rb, uint32_t slot_index);

#if RB_ENABLE_STATS
const rb_stats_t *rb_stats(const rb_t *rb);
void rb_stats_reset(rb_t *rb);
#endif

/* ---------------- Runtime reconfig ----------------
 * Sizes that change ABI (capacity, slots, slot_size) are frozen at init.
 * limit can change but only while the ring is empty.
 * Low-water percents, policy and callbacks can change at any time.
 * Stack-size setters are read by rb_thread_spawn; changing them after
 * a thread has been spawned has no effect on that thread.
 */
rb_err_t  rb_set_limit(rb_t *rb, uint32_t limit);
rb_err_t  rb_set_low_d(rb_t *rb, uint32_t percent);
rb_err_t  rb_set_low_e(rb_t *rb, uint32_t percent);
rb_err_t  rb_set_oversize_policy(rb_t *rb, rb_oversize_policy_t p);
rb_err_t  rb_set_callbacks(rb_t *rb, const rb_callbacks_t *cb);
rb_err_t  rb_set_producer_stack(rb_t *rb, size_t bytes);
rb_err_t  rb_set_consumer_stack(rb_t *rb, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif /* RB_H */
