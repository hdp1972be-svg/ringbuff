#ifndef RB_PORT_H
#define RB_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "rb_config.h"

#if RB_USE_ATOMICS
#  include <stdatomic.h>
   typedef _Atomic uint32_t rb_atomic_u32;
#  define RB_ATOMIC_LOAD_ACQ(p)    atomic_load_explicit((p), memory_order_acquire)
#  define RB_ATOMIC_LOAD_RLX(p)    atomic_load_explicit((p), memory_order_relaxed)
#  define RB_ATOMIC_STORE_REL(p,v) atomic_store_explicit((p),(v),memory_order_release)
#  define RB_ATOMIC_STORE_RLX(p,v) atomic_store_explicit((p),(v),memory_order_relaxed)
#else
   typedef volatile uint32_t rb_atomic_u32;
#  if RB_SINGLE_THREADED
#    define RB_ATOMIC_LOAD_ACQ(p)    (*(p))
#    define RB_ATOMIC_LOAD_RLX(p)    (*(p))
#    define RB_ATOMIC_STORE_REL(p,v) do { *(p)=(v); } while (0)
#    define RB_ATOMIC_STORE_RLX(p,v) do { *(p)=(v); } while (0)
#  elif defined(__GNUC__) || defined(__clang__)
#    define RB_ATOMIC_LOAD_ACQ(p)    (*(p))
#    define RB_ATOMIC_LOAD_RLX(p)    (*(p))
#    define RB_ATOMIC_STORE_REL(p,v) do { __asm__ volatile("" ::: "memory"); *(p)=(v); } while (0)
#    define RB_ATOMIC_STORE_RLX(p,v) do { *(p)=(v); } while (0)
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
