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

/* =========================================================================
 * Hardware / device visibility stubs
 * =========================================================================
 *
 * When the scratchpad lives in ordinary coherent DRAM these macros are
 * complete no-ops (zero cost).  Override them when the payload memory is
 * visible to a GPU, FPGA, DMA engine, dual-port BRAM, or another bus
 * master that does not share the CPU's cache hierarchy.
 *
 * Typical dual-ring offload picture:
 *
 *   CPU  --publish-->  Ring A (scratchpad in device-visible mem)  --> device
 *   CPU  <--consume--  Ring B (device writes results here)        <-- device
 *
 * The ring protocol only handles ownership.  These hooks handle the
 * platform-specific "make my writes visible" / "see the device's writes"
 * part.
 *
 * Override examples (compile with -D or put real functions in a platform
 * file and #define the macros to call them):
 *
 *   // CUDA managed / pinned host memory
 *   #define RB_HW_FLUSH_SLOT(rb, idx)      cuda_flush_slot((rb), (idx))
 *   #define RB_HW_INVALIDATE_SLOT(rb, idx) cuda_invalidate_slot((rb), (idx))
 *
 *   // FPGA / AXI dual-port BRAM (non-coherent)
 *   #define RB_HW_FLUSH_SLOT(rb, idx)      \
 *       do { dsb(sy); /* or writel fence */ } while (0)
 *   #define RB_HW_INVALIDATE_SLOT(rb, idx) \
 *       do { /* invalidate CPU cache lines of the slot */ } while (0)
 *
 *   // DMA engine that will read the slot after publish
 *   #define RB_HW_FLUSH_SLOT(rb, idx)      dma_sync_for_device((rb), (idx))
 *   #define RB_HW_INVALIDATE_SLOT(rb, idx) dma_sync_for_cpu((rb), (idx))
 *
 *   // Optional doorbell after a successful publish
 *   #define RB_HW_NOTIFY_DEVICE(rb, idx, len, trunc) \
 *       writel(1, my_fpga_doorbell_reg)
 *
 * The macros receive the rb_t* and the slot index so an implementation can
 * compute the absolute address from rb_slot_ptr() / the relative scratch
 * offset if needed.
 */

#ifndef RB_HW_FLUSH_SLOT
/* Producer side — called just before the release-store that publishes a
 * slot.  Guarantee that all CPU writes to the slot payload (and header)
 * are visible to the device that will read them.
 *
 * Default: nothing.
 *
 * Pseudo-code for a real override:
 *
 *   void my_flush(rb_t *rb, uint32_t idx) {
 *       void *p = rb_slot_ptr(rb, idx);
 *       size_t n = rb->slot_size;          // or exact written length
 *       cache_clean(p, n);                 // platform clean/flush
 *       dma_wmb();                         // or dsb(), sfence, …
 *   }
 *   #define RB_HW_FLUSH_SLOT(rb, idx) my_flush((rb), (idx))
 */
#  define RB_HW_FLUSH_SLOT(rb, slot_index) ((void)0)
#endif

#ifndef RB_HW_INVALIDATE_SLOT
/* Consumer side — called after a successful consume, before the user is
 * given the payload pointer.  Guarantee that the CPU can see whatever
 * the device wrote into the slot.
 *
 * Default: nothing.
 *
 * Pseudo-code for a real override:
 *
 *   void my_invalidate(rb_t *rb, uint32_t idx) {
 *       void *p = rb_slot_ptr(rb, idx);
 *       size_t n = rb->slot_size;
 *       cache_invalidate(p, n);            // platform invalidate
 *       dma_rmb();                         // or dsb(), lfence, …
 *   }
 *   #define RB_HW_INVALIDATE_SLOT(rb, idx) my_invalidate((rb), (idx))
 */
#  define RB_HW_INVALIDATE_SLOT(rb, slot_index) ((void)0)
#endif

#ifndef RB_HW_NOTIFY_DEVICE
/* Optional doorbell — called after a slot has been published and the
 * release-store is visible.  Use it to kick a GPU kernel, raise an
 * MSI-X, write an FPGA doorbell register, start a DMA, etc.
 *
 * Default: nothing.
 *
 * This is independent of the Linux futex/eventfd path (which only wakes
 * CPU waiters).  You can also put the same kick inside your own
 * on_slot_added callback; the macro simply gives a single, obvious
 * place for device-side signalling.
 *
 * Pseudo-code:
 *
 *   void my_doorbell(rb_t *rb, uint32_t idx, uint32_t len, bool trunc) {
 *       (void)rb; (void)trunc;
 *       my_fpga->slot   = idx;
 *       my_fpga->length = len;
 *       writel(1, &my_fpga->kick);         // or cudaEventRecord(…)
 *   }
 *   #define RB_HW_NOTIFY_DEVICE(rb, idx, len, trunc) \
 *       my_doorbell((rb), (idx), (len), (trunc))
 */
#  define RB_HW_NOTIFY_DEVICE(rb, slot_index, len, truncated) ((void)0)
#endif

#endif /* RB_PORT_H */
