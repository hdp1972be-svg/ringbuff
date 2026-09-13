/* SPDX-License-Identifier: MIT */
#include "rb.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    rb_config_t cfg;
    rb_t *rb;
    uint8_t *scratch;
    uint32_t held = UINT32_MAX, cap = 0;
    void *writable = NULL;
    size_t pos = 0;

    if (!data || size == 0)
        return 0;

    rb_config_init(&cfg);
    rb_config_set_capacity(&cfg, 8);
    rb_config_set_slots(&cfg, 8);
    rb_config_set_slot_size(&cfg, 64);

    rb = calloc(1, rb_size(8));
    scratch = calloc(8, 64);
    if (!rb || !scratch) {
        free(rb); free(scratch); return 0;
    }
    if (rb_init(rb, &cfg, scratch, 8u * 64u) != RB_OK) {
        free(rb); free(scratch); return 0;
    }

    while (pos < size) {
        unsigned op = data[pos++] & 3u;
        if (held != UINT32_MAX) {
            if (op == 0) {
                uint32_t n = data[pos++ % size] % (cap + 1u);
                if (n) {
                    memset(writable, (int)data[0], n);
                    (void)rb_publish(rb, held, n);
                } else {
                    (void)rb_abort(rb);
                }
            } else {
                (void)rb_abort(rb);
            }
            held = UINT32_MAX;
            writable = NULL;
            cap = 0;
            continue;
        }

        if (op == 0) {
            uint32_t wanted = data[pos++ % size] % 96u;
            (void)rb_acquire(rb, wanted, &held, &writable, &cap);
        } else if (op == 1) {
            uint32_t slot, len;
            const void *obj;
            bool truncated;
            if (rb_consume(rb, &slot, &obj, &len, &truncated) == RB_OK)
                (void)rb_release(rb, slot);
        } else if (op == 2) {
            (void)rb_drain(rb, NULL, NULL);
        } else {
            (void)rb_count(rb);
            (void)rb_capacity(rb);
            (void)rb_limit(rb);
            (void)rb_is_empty(rb);
            (void)rb_is_full(rb);
        }

        if (rb_count(rb) > rb_capacity(rb))
            abort();
    }

    if (held != UINT32_MAX)
        (void)rb_abort(rb);
    if (rb_count(rb) > rb_capacity(rb))
        abort();
    rb_deinit(rb);
    free(scratch);
    free(rb);
    return 0;
}
