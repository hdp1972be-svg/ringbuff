/* SPDX-License-Identifier: MIT */
/* Dual-ring shared-memory layout for the Zynq offload example. */
#ifndef ZYNQ_OFFLOAD_COMMON_H
#define ZYNQ_OFFLOAD_COMMON_H

#include "rb.h"

#include <stdint.h>
#include <stddef.h>

/* ---- Tunables (keep small so the example fits in a modest BRAM) ---- */
#define ZO_CAPACITY     32u
#define ZO_SLOTS        32u
#define ZO_SLOT_SIZE    512u   /* header + ~500 B of WS JSON */
#define ZO_SHM_NAME     "/rb_zynq_offload"

/* Result written by the FPGA into the egress ring.
 * Layout is fixed and 4-byte aligned so it is safe to cast the
 * zero-copy slot payload pointer (which sits at RB_SLOT_HDR_SIZE
 * past a cache-line-aligned slot base). */
struct zo_result {
    uint32_t seq;
    uint32_t hash;
    uint32_t in_len;
    char     tag[4];   /* "HASH" — not NUL-terminated */
};

_Static_assert(sizeof(struct zo_result) == 16, "zo_result must stay 16 bytes");
_Static_assert(_Alignof(struct zo_result) == 4, "zo_result must be 4-byte aligned");

/*
 * Shared region layout (one POSIX shm object, or one reserved DDR/BRAM
 * window on the real board):
 *
 *   [ doorbell words ]
 *   [ ring A control  ]  CPU → FPGA   (ingress)   — RB_CACHE_LINE aligned
 *   [ ring B control  ]  FPGA → CPU   (egress)    — RB_CACHE_LINE aligned
 *   [ scratch A       ]  ZO_SLOTS * ZO_SLOT_SIZE  — RB_CACHE_LINE aligned
 *   [ scratch B       ]  ZO_SLOTS * ZO_SLOT_SIZE  — RB_CACHE_LINE aligned
 *
 * Doorbell words are ordinary uint32_t flags the other side can poll or
 * that an IRQ controller can watch.  On the real S9 you would map these
 * to AXI-lite registers instead.
 *
 * Ring control blocks and scratchpads MUST be aligned to RB_CACHE_LINE
 * (and therefore to _Alignof(rb_t)); rb_init rejects a misaligned control
 * block.  Plain struct packing would place ring_a_mem at offset 16.
 */
struct zo_doorbells {
    volatile uint32_t cpu_to_fpga;   /* CPU wrote a new ingress slot */
    volatile uint32_t fpga_to_cpu;   /* FPGA wrote a new egress slot */
    volatile uint32_t seq_in;        /* free-running input sequence */
    volatile uint32_t seq_out;       /* free-running output sequence */
};

struct zo_shared {
    struct zo_doorbells bell;
    _Alignas(RB_CACHE_LINE) uint8_t ring_a_mem[4096];
    _Alignas(RB_CACHE_LINE) uint8_t ring_b_mem[4096];
    _Alignas(RB_CACHE_LINE) uint8_t scratch_a[(size_t)ZO_SLOTS * ZO_SLOT_SIZE];
    _Alignas(RB_CACHE_LINE) uint8_t scratch_b[(size_t)ZO_SLOTS * ZO_SLOT_SIZE];
};

static inline rb_t *zo_ring_a(struct zo_shared *s)
{
    return (rb_t *)s->ring_a_mem;
}

static inline rb_t *zo_ring_b(struct zo_shared *s)
{
    return (rb_t *)s->ring_b_mem;
}

/* Simple 32-bit rolling hash — stand-in for AES/SHA/zip on the FPGA. */
static inline uint32_t zo_fast_hash(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5u; /* FNV-1a offset basis */
    for (uint32_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

#endif /* ZYNQ_OFFLOAD_COMMON_H */
