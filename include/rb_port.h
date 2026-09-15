/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */
#ifndef RB_PORT_H
#define RB_PORT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "rb_config.h"

/* Optional platform overrides. */
#ifndef RB_MEMCPY
#  define RB_MEMCPY memcpy
#endif
#ifndef RB_MEMSET
#  define RB_MEMSET memset
#endif

/* rb.c includes this low-level port header before using memcpy/memset.
 * Redirect those operations here so both CMake and direct builds honor the
 * hooks without requiring edits to the implementation source. With the
 * defaults this expands back to the normal libc functions. */
#define memcpy RB_MEMCPY
#define memset RB_MEMSET

#if RB_USE_ATOMICS
#  include <stdatomic.h>
   typedef _Atomic uint32_t rb_atomic_u32;
   typedef _Atomic uint64_t rb_atomic_u64;
#  define RB_ATOMIC_LOAD_ACQ(p)    atomic_load_explicit((p), memory_order_acquire)
#  define RB_ATOMIC_LOAD_RLX(p)    atomic_load_explicit((p), memory_order_relaxed)
#  define RB_ATOMIC_STORE_REL(p,v) atomic_store_explicit((p),(v), memory_order_release)
#  define RB_ATOMIC_STORE_RLX(p,v) atomic_store_explicit((p),(v), memory_order_relaxed)
#  define RB_ATOMIC_FETCH_ADD(p,v) atomic_fetch_add_explicit((p),(v),memory_order_relaxed)
#  define RB_ATOMIC_FETCH_SUB(p,v) atomic_fetch_sub_explicit((p),(v),memory_order_relaxed)
#  define RB_ATOMIC_LOAD_U64_ACQ(p) atomic_load_explicit((p), memory_order_acquire)
#  define RB_ATOMIC_STORE_U64_RLX(p,v) atomic_store_explicit((p),(v), memory_order_relaxed)
#  define RB_ATOMIC_FETCH_ADD_U64(p,v) atomic_fetch_add_explicit((p),(v),memory_order_relaxed)
#else
   typedef volatile uint32_t rb_atomic_u32;
   typedef volatile uint64_t rb_atomic_u64;
#  if RB_SINGLE_THREADED
#    define RB_ATOMIC_LOAD_ACQ(p)    (*(p))
#    define RB_ATOMIC_LOAD_RLX(p)    (*(p))
#    define RB_ATOMIC_STORE_REL(p,v) do { *(p)=(v); } while (0)
#    define RB_ATOMIC_STORE_RLX(p,v) do { *(p)=(v); } while (0)
#    define RB_ATOMIC_FETCH_ADD(p,v) (*(p) += (v), *(p) - (v))
#    define RB_ATOMIC_FETCH_SUB(p,v) (*(p) -= (v), *(p) + (v))
#    define RB_ATOMIC_LOAD_U64_ACQ(p) (*(p))
#    define RB_ATOMIC_STORE_U64_RLX(p,v) do { *(p)=(v); } while (0)
#    define RB_ATOMIC_FETCH_ADD_U64(p,v) (*(p) += (v), *(p) - (v))
#  elif defined(__GNUC__) || defined(__clang__)
#    define RB_ATOMIC_LOAD_ACQ(p)    (*(p))
#    define RB_ATOMIC_LOAD_RLX(p)    (*(p))
#    define RB_ATOMIC_STORE_REL(p,v) do { __asm__ volatile("" ::: "memory"); *(p)=(v); } while (0)
#    define RB_ATOMIC_STORE_RLX(p,v) do { *(p)=(v); } while (0)
#    define RB_ATOMIC_FETCH_ADD(p,v) __atomic_fetch_add((p),(v),__ATOMIC_RELAXED)
#    define RB_ATOMIC_FETCH_SUB(p,v) __atomic_fetch_sub((p),(v),__ATOMIC_RELAXED)
#    define RB_ATOMIC_LOAD_U64_ACQ(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#    define RB_ATOMIC_STORE_U64_RLX(p,v) __atomic_store_n((p),(v),__ATOMIC_RELAXED)
#    define RB_ATOMIC_FETCH_ADD_U64(p,v) __atomic_fetch_add((p),(v),__ATOMIC_RELAXED)
#  else
#    error "No atomic implementation available for this target"
#  endif
#endif
#define RB_ALIGN_UP(x,a) (((x) + ((size_t)((a)-1u))) & ~((size_t)((a)-1u)))
#define RB_IS_POW2(x)    ((x) != 0u && (((x) & ((x)-1u)) == 0u))
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define RB_ALIGNAS(n) _Alignas(n)
#else
#  define RB_ALIGNAS(n) __attribute__((aligned(n)))
#endif

/* ---------------- Prefetch ----------------
 * RB_PREFETCH_R(p) hints that *p will be read soon.
 * RB_PREFETCH_W(p) hints that *p will be written soon.
 *
 * On x86 and ARM these compile to a single instruction. On targets
 * without a prefetch instruction they compile to nothing. The hint is
 * advisory; correctness never depends on it.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define RB_PREFETCH_R(p) __builtin_prefetch((const void *)(p), 0, 3)
#  define RB_PREFETCH_W(p) __builtin_prefetch((const void *)(p), 1, 3)
#else
#  define RB_PREFETCH_R(p) ((void)0)
#  define RB_PREFETCH_W(p) ((void)0)
#endif

#endif /* RB_PORT_H */
