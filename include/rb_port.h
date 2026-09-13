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
#  define RB_ATOMIC_LOAD_ACQ(p)    atomic_load_explicit((p), memory_order_acquire)
#  define RB_ATOMIC_LOAD_RLX(p)    atomic_load_explicit((p), memory_order_relaxed)
#  define RB_ATOMIC_STORE_REL(p,v) atomic_store_explicit((p),(v), memory_order_release)
#  define RB_ATOMIC_STORE_RLX(p,v) atomic_store_explicit((p),(v), memory_order_relaxed)
#  define RB_ATOMIC_FETCH_ADD(p,v) atomic_fetch_add_explicit((p),(v),memory_order_relaxed)
#  define RB_ATOMIC_FETCH_SUB(p,v) atomic_fetch_sub_explicit((p),(v),memory_order_relaxed)
#else
   typedef volatile uint32_t rb_atomic_u32;
#  if RB_SINGLE_THREADED
#    define RB_ATOMIC_LOAD_ACQ(p)    (*(p))
#    define RB_ATOMIC_LOAD_RLX(p)    (*(p))
#    define RB_ATOMIC_STORE_REL(p,v) do { *(p)=(v); } while (0)
#    define RB_ATOMIC_STORE_RLX(p,v) do { *(p)=(v); } while (0)
#    define RB_ATOMIC_FETCH_ADD(p,v) (*(p) += (v), *(p) - (v))
#    define RB_ATOMIC_FETCH_SUB(p,v) (*(p) -= (v), *(p) + (v))
#  elif defined(__GNUC__) || defined(__clang__)
#    define RB_ATOMIC_LOAD_ACQ(p)    (*(p))
#    define RB_ATOMIC_LOAD_RLX(p)    (*(p))
#    define RB_ATOMIC_STORE_REL(p,v) do { __asm__ volatile("" ::: "memory"); *(p)=(v); } while (0)
#    define RB_ATOMIC_STORE_RLX(p,v) do { *(p)=(v); } while (0)
#    define RB_ATOMIC_FETCH_ADD(p,v) __atomic_fetch_add((p),(v),__ATOMIC_RELAXED)
#    define RB_ATOMIC_FETCH_SUB(p,v) __atomic_fetch_sub((p),(v),__ATOMIC_RELAXED)
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
#endif /* RB_PORT_H */
