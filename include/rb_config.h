/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

#ifndef RB_CONFIG_H
#define RB_CONFIG_H

/* Slot size in bytes, including the 4-byte header. */
#ifndef RB_SLOT_SIZE
#define RB_SLOT_SIZE          2048u
#endif

/* Default scratchpad slot count (z). Runtime-overridable. */
#ifndef RB_NUM_SLOTS
#define RB_NUM_SLOTS          64u
#endif

/* Default ring capacity. Must be a power of two. */
#ifndef RB_CAPACITY
#define RB_CAPACITY           64u
#endif

/* Cache line size. Used for control-block separation and slot padding. */
#ifndef RB_CACHE_LINE
#define RB_CACHE_LINE         64u
#endif

/* 0 = uint32_t slot index (default), 1 = void* slot pointer. */
#ifndef RB_USE_POINTERS
#define RB_USE_POINTERS       0
#endif

/* 1 = pad slot stride up to RB_CACHE_LINE to avoid false sharing. */
#ifndef RB_SLOT_CACHELINE_PAD
#define RB_SLOT_CACHELINE_PAD 0
#endif

/* 1 = no atomics, no barriers. For single-loop / bare-metal use. */
#ifndef RB_SINGLE_THREADED
#define RB_SINGLE_THREADED    0
#endif

/* If single-threaded, force atomics off (overrides external -D). */
#if RB_SINGLE_THREADED
#undef  RB_USE_ATOMICS
#define RB_USE_ATOMICS        0
#endif

/* 1 = use C11 <stdatomic.h>, 0 = porting-layer barriers only. */
#ifndef RB_USE_ATOMICS
#define RB_USE_ATOMICS        1
#endif

/* 1 = compile stats (watermarks, counters) into the control block. */
#ifndef RB_ENABLE_STATS
#define RB_ENABLE_STATS       1
#endif

/* 1 = build pthread spawn/join helper. Off for no-OS targets. */
#ifndef RB_ENABLE_THREAD_HELPERS
#define RB_ENABLE_THREAD_HELPERS 1
#endif

/* 1 = Linux futex + eventfd notification support. Default off so the
   normal build remains POSIX/Linux-independent and adds no notify state. */
#ifndef RB_ENABLE_NOTIFY
#define RB_ENABLE_NOTIFY      0
#endif

/* Low-water defaults, percentages of `limit`. 0 disables a threshold. */
#ifndef RB_DEFAULT_LOW_D
#define RB_DEFAULT_LOW_D      25u
#endif
#ifndef RB_DEFAULT_LOW_E
#define RB_DEFAULT_LOW_E      10u
#endif

/* Default thread stack sizes (bytes) used by rb_thread_spawn. */
#ifndef RB_DEFAULT_PRODUCER_STACK
#define RB_DEFAULT_PRODUCER_STACK (256u * 1024u)
#endif
#ifndef RB_DEFAULT_CONSUMER_STACK
#define RB_DEFAULT_CONSUMER_STACK (256u * 1024u)
#endif

/* Floor for thread stack sizes. Linux PTHREAD_STACK_MIN is often 16K;
   Android's effective floor is larger. */
#ifndef RB_THREAD_MIN_STACK
#define RB_THREAD_MIN_STACK   (64u * 1024u)
#endif

#endif /* RB_CONFIG_H */
