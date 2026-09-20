/* SPDX-License-Identifier: MIT */
/* Dual-ring shared-memory layout for the Zynq offload example. */
#ifndef ZYNQ_OFFLOAD_COMMON_H
#define ZYNQ_OFFLOAD_COMMON_H

#include "rb.h"

#include <stdint.h>
#include <stddef.h>

/* ---- Tunables (host test allows larger; real BRAM should stay modest) ---- */
#define ZO_CAPACITY     32u
#define ZO_SLOTS        32u
#define ZO_SLOT_SIZE    8192u  /* allows -s up to ~8 KiB payloads in host test */
#define ZO_SHM_NAME     "/rb_zynq_offload"

/* Result written by the FPGA into the egress ring. */
struct zo_result {
    uint32_t seq;
    uint32_t hash;
    uint32_t in_len;
    char     tag[4];   /* "HASH" */
};

/*
 * Shared region layout (one POSIX shm object, or one reserved DDR/BRAM
 * window on the real board):
 *
 *   [ doorbell words ]
 *   [ ring A control  ]  CPU → FPGA   (ingress)
 *   [ ring B control  ]  FPGA → CPU   (egress)
 *   [ scratch A       ]  ZO_SLOTS * ZO_SLOT_SIZE
 *   [ scratch B       ]  ZO_SLOTS * ZO_SLOT_SIZE
 *
 * Doorbell words are ordinary uint32_t flags the other side can poll or
 * that an IRQ controller can watch.  On the real S9 you would map these
 * to AXI-lite registers instead.
 */
struct zo_doorbells {
    volatile uint32_t cpu_to_fpga;   /* CPU wrote a new ingress slot */
    volatile uint32_t fpga_to_cpu;   /* FPGA wrote a new egress slot */
    volatile uint32_t seq_in;        /* free-running input sequence */
    volatile uint32_t seq_out;       /* free-running output sequence */
};

struct zo_shared {
    struct zo_doorbells bell;
    /* Control blocks must be RB_CACHE_LINE aligned (rb_t has ALIGNAS). */
    uint8_t _pad0[64 - (sizeof(struct zo_doorbells) % 64)];
    uint8_t ring_a_mem[4096] __attribute__((aligned(64)));
    uint8_t ring_b_mem[4096] __attribute__((aligned(64)));
    uint8_t scratch_a[(size_t)ZO_SLOTS * ZO_SLOT_SIZE] __attribute__((aligned(64)));
    uint8_t scratch_b[(size_t)ZO_SLOTS * ZO_SLOT_SIZE] __attribute__((aligned(64)));
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
