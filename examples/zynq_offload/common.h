/* SPDX-License-Identifier: MIT */
/* Dual-ring shared-memory layout for the Zynq offload example. */
#ifndef ZYNQ_OFFLOAD_COMMON_H
#define ZYNQ_OFFLOAD_COMMON_H

#include "rb.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

/*
 * Slot / capacity sizing
 * ---------------------
 * Scratchpads are allocated for ZO_MAX_SLOTS so -s can pick any size up
 * to that at rb_init time (rb_config_set_capacity / rb_config_set_slots).
 * The host writes the chosen value into the shared header; the FPGA
 * side only observes it (it does not re-init the rings).
 *
 * Default remains 32 for small-BRAM demos; raise with -s for fewer drops
 * under a fast host.
 */
#define ZO_DEFAULT_SLOTS  32u
#define ZO_MAX_SLOTS      4096u
#define ZO_SLOT_SIZE      512u   /* JSON frame or future protobuf/avro blob */
#define ZO_RING_MEM       32768u /* room for rb_size(ZO_MAX_SLOTS) */
#define ZO_SHM_NAME       "/rb_zynq_offload"

/* ANSI colours for packet dumps (red = written, green = received). */
#define ZO_CLR_RED      "\033[31m"
#define ZO_CLR_GREEN    "\033[32m"
#define ZO_CLR_RESET    "\033[0m"

/*
 * Ingress message (CPU → FPGA)
 * ---------------------------
 * Written as a NUL-terminated JSON string into the ring slot so the
 * host-test path stays human-readable.  Shape:
 *
 *   {"ts":<uint64_ns>,"seq":<uint32>,"payload":"..."}
 *
 * Next implementation: replace the JSON body with protobuf or Avro
 * while keeping the same ring protocol and slot size.
 */

/* Result written by the FPGA into the egress ring. */
struct zo_result {
    uint32_t seq;          /* echoes ingress seq */
    uint32_t hash;         /* processed payload hash */
    uint32_t in_len;       /* original ingress byte length */
    uint32_t _pad;         /* keep 8-byte alignment for ts fields */
    uint64_t ts_in;        /* ingress ts (ns) parsed from JSON */
    uint64_t ts_fpga;      /* local time when FPGA finished work */
    char     tag[4];       /* "HASH" — not NUL-terminated */
    char     payload[60];  /* short echo of ingress payload for display */
};

_Static_assert(sizeof(struct zo_result) == 96, "zo_result size drift");
_Static_assert(_Alignof(struct zo_result) >= 8, "zo_result alignment");

/*
 * Shared region layout:
 *
 *   [ doorbell + config words ]
 *   [ ring A control  ]  CPU → FPGA   (ingress)
 *   [ ring B control  ]  FPGA → CPU   (egress)
 *   [ scratch A       ]  ZO_MAX_SLOTS * ZO_SLOT_SIZE
 *   [ scratch B       ]  ZO_MAX_SLOTS * ZO_SLOT_SIZE
 *
 * Actual capacity/slots used is min(requested, ZO_MAX_SLOTS) and is
 * recorded in bell.slots after the host calls rb_init.
 */
struct zo_doorbells {
    volatile uint32_t cpu_to_fpga;   /* CPU wrote a new ingress slot */
    volatile uint32_t fpga_to_cpu;   /* FPGA wrote a new egress slot */
    volatile uint32_t seq_in;        /* free-running input sequence */
    volatile uint32_t seq_out;       /* free-running output sequence */
    volatile uint32_t slots;         /* capacity == slots used by both rings */
    volatile uint32_t ready;         /* 1 once host finished rb_init */
};

struct zo_shared {
    struct zo_doorbells bell;
    _Alignas(RB_CACHE_LINE) uint8_t ring_a_mem[ZO_RING_MEM];
    _Alignas(RB_CACHE_LINE) uint8_t ring_b_mem[ZO_RING_MEM];
    _Alignas(RB_CACHE_LINE) uint8_t scratch_a[(size_t)ZO_MAX_SLOTS * ZO_SLOT_SIZE];
    _Alignas(RB_CACHE_LINE) uint8_t scratch_b[(size_t)ZO_MAX_SLOTS * ZO_SLOT_SIZE];
};

static inline rb_t *zo_ring_a(struct zo_shared *s)
{
    return (rb_t *)s->ring_a_mem;
}

static inline rb_t *zo_ring_b(struct zo_shared *s)
{
    return (rb_t *)s->ring_b_mem;
}

/* Clamp requested slots into [1, ZO_MAX_SLOTS]. */
static inline uint32_t zo_clamp_slots(uint32_t n)
{
    if (n < 1u)
        return 1u;
    if (n > ZO_MAX_SLOTS)
        return ZO_MAX_SLOTS;
    return n;
}

static inline uint64_t zo_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline double zo_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline uint32_t zo_fast_hash(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static inline void zo_print_sep(void)
{
    fputs("---\n", stdout);
}

#endif /* ZYNQ_OFFLOAD_COMMON_H */
