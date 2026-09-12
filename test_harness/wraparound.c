/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

/*
 * Counter-wrap test harness.
 *
 * This intentionally includes the implementation so the opaque rb_t can be
 * positioned immediately before UINT32_MAX. No production source is changed.
 * The test therefore exercises the real uint32_t sequence arithmetic rather
 * than a reduced-width surrogate.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/rb.c"

static rb_t *make_rb(void **control_mem, void **scratch_mem)
{
    rb_config_t cfg;
    rb_config_init(&cfg);
    rb_config_set_capacity(&cfg, 4);
    rb_config_set_limit(&cfg, 4);
    rb_config_set_slots(&cfg, 4);
    rb_config_set_slot_size(&cfg, 16);

    size_t control_size = rb_size(4);
    size_t control_alloc = (control_size + 63u) & ~((size_t)63u);
    *control_mem = aligned_alloc(64, control_alloc);
    assert(*control_mem != NULL);

    size_t scratch_size = 4u * 16u;
    size_t scratch_alloc = (scratch_size + 63u) & ~((size_t)63u);
    *scratch_mem = aligned_alloc(64, scratch_alloc);
    assert(*scratch_mem != NULL);

    rb_t *rb = (rb_t *)*control_mem;
    assert(rb_init(rb, &cfg, *scratch_mem, scratch_size) == RB_OK);
    return rb;
}

static void reset_counters(rb_t *rb, uint32_t sequence)
{
    RB_ATOMIC_STORE_RLX(&rb->head, sequence);
    RB_ATOMIC_STORE_RLX(&rb->tail, sequence);
    rb->cached_tail = sequence;
    rb->cached_head = sequence;
    rb->pending_slot = RB_NO_PENDING;
    rb->consumer_active = 0;
}

static void put(rb_t *rb, uint32_t value, uint32_t expected_slot)
{
    uint32_t slot, cap;
    void *dst;
    assert(rb_acquire(rb, sizeof(value), &slot, &dst, &cap) == RB_OK);
    assert(slot == expected_slot);
    assert(cap >= sizeof(value));
    memcpy(dst, &value, sizeof(value));
    assert(rb_publish(rb, slot, sizeof(value)) == RB_OK);
}

static uint32_t get(rb_t *rb, uint32_t expected_value, uint32_t expected_slot)
{
    uint32_t slot, len;
    const void *obj;
    bool truncated;
    assert(rb_consume(rb, &slot, &obj, &len, &truncated) == RB_OK);
    assert(slot == expected_slot);
    assert(len == sizeof(expected_value));
    assert(!truncated);
    uint32_t value;
    memcpy(&value, obj, sizeof(value));
    assert(value == expected_value);
    assert(rb_release(rb, slot) == RB_OK);
    return value;
}

int main(void)
{
    void *control = NULL;
    void *scratch = NULL;
    rb_t *rb = make_rb(&control, &scratch);

    /*
     * Start four positions before UINT32_MAX. Four publishes therefore move
     * head through 0, while the four physical slots wrap independently.
     */
    const uint32_t start = UINT32_MAX - 3u;
    reset_counters(rb, start);

    assert(rb_count(rb) == 0);
    assert(rb_is_empty(rb));

    put(rb, 3, start & rb->mask);
    assert(rb_count(rb) == 1);
    put(rb, 4, (start + 1u) & rb->mask);
    assert(rb_count(rb) == 2);
    put(rb, 5, (start + 2u) & rb->mask);
    assert(rb_count(rb) == 3);
    put(rb, 6, (start + 3u) & rb->mask);
    assert(rb_count(rb) == 4);
    assert(rb_is_full(rb));

    /* head must have wrapped exactly to zero. */
    assert(RB_ATOMIC_LOAD_RLX(&rb->head) == 0u);
    assert(RB_ATOMIC_LOAD_RLX(&rb->tail) == start);
    assert(rb_count(rb) == 4u);

    /* FIFO survives the physical slot wrap. */
    get(rb, 3, start & rb->mask);
    assert(rb_count(rb) == 3);
    get(rb, 4, (start + 1u) & rb->mask);
    assert(rb_count(rb) == 2);
    get(rb, 5, (start + 2u) & rb->mask);
    assert(rb_count(rb) == 1);
    get(rb, 6, (start + 3u) & rb->mask);
    assert(rb_count(rb) == 0);
    assert(rb_is_empty(rb));
    assert(RB_ATOMIC_LOAD_RLX(&rb->tail) == 0u);

    /*
     * Now test the mixed case: counters wrap while the queue is non-empty.
     * This is where unsigned subtraction and cached opposite-side counters
     * matter most.
     */
    reset_counters(rb, UINT32_MAX - 2u);

    put(rb, 10, (UINT32_MAX - 2u) & rb->mask);
    put(rb, 11, (UINT32_MAX - 1u) & rb->mask);
    put(rb, 12, UINT32_MAX & rb->mask);
    assert(rb_count(rb) == 3u);
    assert(RB_ATOMIC_LOAD_RLX(&rb->head) == 0u);

    /* Free one slot after head wrapped, then refill it. */
    get(rb, 10, (UINT32_MAX - 2u) & rb->mask);
    assert(rb_count(rb) == 2u);
    put(rb, 13, 0u);
    assert(rb_count(rb) == 3u);

    get(rb, 11, (UINT32_MAX - 1u) & rb->mask);
    get(rb, 12, UINT32_MAX & rb->mask);
    get(rb, 13, 0u);
    assert(rb_count(rb) == 0u);
    assert(rb_is_empty(rb));

    /* Full detection immediately across the modulo boundary. */
    reset_counters(rb, UINT32_MAX - 1u);
    for (uint32_t i = 0; i < 4; ++i)
        put(rb, 100u + i, (UINT32_MAX - 1u + i) & rb->mask);
    assert(rb_count(rb) == 4u);
    uint32_t slot, cap;
    void *dst;
    assert(rb_acquire(rb, 4, &slot, &dst, &cap) == RB_ERR_FULL);

    rb_deinit(rb);
    free(scratch);
    free(control);

    puts("PASS: uint32_t head/tail wraparound, FIFO, count, full/empty, and cached-counter paths");
    return 0;
}
