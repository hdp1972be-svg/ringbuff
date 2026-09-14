/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */
#ifndef RB_H
#define RB_H
#include "rb_config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RB_VERSION_MAJOR 1
#define RB_VERSION_MINOR 0
#define RB_VERSION_PATCH 0
#define RB_VERSION_STRING "1.0.0"
typedef enum {
    RB_OK = 0,
    RB_ERR_FULL,
    RB_ERR_EMPTY,
    RB_ERR_OVERSIZE,
    RB_ERR_INVAL,
    RB_ERR_NOT_INIT
} rb_err_t;
#define RB_SLOT_TRUNCATED 0x80000000u
#define RB_SLOT_LEN_MASK 0x7FFFFFFFu
#define RB_SLOT_HDR_SIZE 4u
typedef enum { RB_OVERSIZE_TRUNCATE = 0, RB_OVERSIZE_DROP = 1 } rb_oversize_policy_t;
#if RB_USE_POINTERS
typedef void *rb_entry_t;
#else
typedef uint32_t rb_entry_t;
#endif
typedef struct rb_s rb_t;
typedef void (*rb_cb_t)(rb_t *rb, void *user);
typedef void (*rb_slot_cb_t)(rb_t *rb, uint32_t slot_index, uint32_t len, bool truncated,
                             void *user);
typedef struct {
    rb_slot_cb_t on_slot_added;
    rb_cb_t on_low_d;
    rb_cb_t on_low_e;
    rb_cb_t on_full;
    void *user;
} rb_callbacks_t;
#if RB_ENABLE_STATS
typedef struct {
    uint64_t published;
    uint64_t consumed;
    uint64_t truncated;
    uint64_t full_attempts;
    uint32_t high_water;
    uint32_t full_hits;
    uint32_t low_d_hits;
    uint32_t low_e_hits;
} rb_stats_t;
#endif
typedef struct {
    uint32_t capacity;
    uint32_t limit;
    uint32_t slots;
    uint32_t slot_size;
    uint32_t low_d;
    uint32_t low_e;
    rb_oversize_policy_t oversize_policy;
    size_t producer_stack_size;
    size_t consumer_stack_size;
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
size_t rb_size(uint32_t capacity);
rb_err_t rb_init(rb_t *rb, const rb_config_t *cfg, void *scratch, size_t scratch_size);
void rb_deinit(rb_t *rb);
size_t rb_producer_stack(const rb_t *rb);
size_t rb_consumer_stack(const rb_t *rb);
rb_err_t rb_acquire(rb_t *rb, uint32_t wanted_len, uint32_t *out_slot_index, void **out_writable,
                    uint32_t *out_cap);
rb_err_t rb_publish(rb_t *rb, uint32_t slot_index, uint32_t written_len);
rb_err_t rb_publish_ex(rb_t *rb, uint32_t slot_index, uint32_t written_len, bool truncated);
rb_err_t rb_abort(rb_t *rb);
rb_err_t rb_consume(rb_t *rb, uint32_t *out_slot_index, const void **out_obj, uint32_t *out_len,
                    bool *out_truncated);
rb_err_t rb_release(rb_t *rb, uint32_t slot_index);
typedef bool (*rb_drain_fn)(const void *obj, uint32_t len, bool truncated, void *user);
uint32_t rb_drain(rb_t *rb, rb_drain_fn fn, void *user);
typedef bool (*rb_flush_fn)(void *writable, uint32_t cap, uint32_t *out_len, bool *out_truncated,
                            bool *out_stop, void *user);
uint32_t rb_flush(rb_t *rb, rb_flush_fn fn, void *user);
uint32_t rb_count(const rb_t *rb);
uint32_t rb_capacity(const rb_t *rb);
uint32_t rb_limit(const rb_t *rb);
bool rb_is_full(const rb_t *rb);
bool rb_is_empty(const rb_t *rb);
void *rb_slot_ptr(rb_t *rb, uint32_t slot_index);
const void *rb_slot_cptr(const rb_t *rb, uint32_t slot_index);
#if RB_ENABLE_STATS
const rb_stats_t *rb_stats(const rb_t *rb);
void rb_stats_reset(rb_t *rb);
#endif
rb_err_t rb_set_limit(rb_t *rb, uint32_t limit);
rb_err_t rb_set_low_d(rb_t *rb, uint32_t percent);
rb_err_t rb_set_low_e(rb_t *rb, uint32_t percent);
rb_err_t rb_set_oversize_policy(rb_t *rb, rb_oversize_policy_t p);
rb_err_t rb_set_callbacks(rb_t *rb, const rb_callbacks_t *cb);
rb_err_t rb_set_producer_stack(rb_t *rb, size_t bytes);
rb_err_t rb_set_consumer_stack(rb_t *rb, size_t bytes);

#if RB_ENABLE_NOTIFY && defined(__linux__)
/* Linux notification support. rb_wait() blocks only when the ring is empty.
 * It uses rb->head itself as the futex sequence word, so publication and
 * notification share one monotonically advancing uint32_t sequence. */
uint32_t rb_notify_value(const rb_t *rb);
int rb_wait(rb_t *rb, uint32_t expected, int timeout_ms);
/* Returns a nonblocking eventfd suitable for poll/epoll/libuv/libev.
 * Call rb_notify_fd() before starting the producer. */
int rb_notify_fd(rb_t *rb);
/* Drain the eventfd counter. Returns 0 on success, -errno on failure. */
int rb_notify_drain_fd(rb_t *rb);
#endif
#ifdef __cplusplus
}
#endif
#endif /* RB_H */
