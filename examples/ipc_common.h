#ifndef RB_IPC_COMMON_H
#define RB_IPC_COMMON_H

/*
 * Shared layout for the two-process IPC example.
 *
 * A single POSIX shared memory region holds both the ring control block
 * and the scratchpad:
 *
 *   offset 0                  rb_t control block  (RB_CACHE_LINE aligned)
 *   offset ipc_scratch_offset()  scratchpad       (RB_CACHE_LINE aligned)
 *   offset ipc_region_size()     end of region
 */

#include "rb.h"
#include <stddef.h>
#include <stdint.h>

#ifndef RB_SHM_NAME
#  define RB_SHM_NAME "/rb_ipc_demo"
#endif
#ifndef RB_CAPACITY
#  define RB_CAPACITY 64u
#endif
#ifndef RB_SLOTS
#  define RB_SLOTS 64u
#endif
#ifndef RB_SLOT_SIZE
#  define RB_SLOT_SIZE 2048u
#endif
#ifndef RB_RUN_SECONDS
#  define RB_RUN_SECONDS 10u
#endif
#ifndef RB_BENCH_MSG_SIZE
#  define RB_BENCH_MSG_SIZE 256u
#endif

static inline size_t ipc_scratch_offset(void) {
    size_t core = rb_size(RB_CAPACITY);
    return (core + RB_CACHE_LINE - 1u) & ~((size_t)RB_CACHE_LINE - 1u);
}

static inline size_t ipc_region_size(void) {
    return ipc_scratch_offset() + (size_t)RB_SLOTS * RB_SLOT_SIZE;
}

static inline rb_t *ipc_ring(void *base) {
    return (rb_t *)base;
}

static inline void *ipc_scratch(void *base) {
    return (uint8_t *)base + ipc_scratch_offset();
}

#endif /* RB_IPC_COMMON_H */
