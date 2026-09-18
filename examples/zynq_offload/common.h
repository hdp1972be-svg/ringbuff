/* SPDX-License-Identifier: MIT */
/* Dual-ring shared-memory layout for the Zynq offload example. */
#ifndef ZYNQ_OFFLOAD_COMMON_H
#define ZYNQ_OFFLOAD_COMMON_H

#include "rb.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* ---- Tunables (keep modest so the example fits in BRAM on a 7010) ---- */
#define ZO_CAPACITY     32u
#define ZO_SLOTS        32u
#define ZO_SLOT_SIZE    512u   /* JSON frame or future protobuf/avro blob */
#define ZO_SHM_NAME     "/rb_zynq_offload"

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
 * `ts`  — CLOCK_REALTIME nanoseconds at publish time
 * `seq` — free-running uint32 packet id
 * `payload` — work item the PL (or fpga_stub) processes
 *
 * Next implementation: replace the JSON body with protobuf or Avro
 * while keeping the same ring protocol and slot size.  Only the
 * serialize/deserialize helpers need to change; the dual-ring path
 * and RB_HW_* hooks stay identical.
 */

/* Result written by the FPGA into the egress ring.
 * Layout is fixed and 8-byte aligned so it is safe to cast the
 * zero-copy slot payload pointer. */
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
 * block.
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

/* Nanoseconds from CLOCK_REALTIME (wall clock, for latency deltas). */
static inline uint64_t zo_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Monotonic seconds for rate reporting. */
static inline double zo_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Simple 32-bit FNV-1a — stand-in for AES/SHA/zip on the FPGA. */
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

/* Packet separator line. */
static inline void zo_print_sep(void)
{
    fputs("---\n", stdout);
}

#endif /* ZYNQ_OFFLOAD_COMMON_H */
