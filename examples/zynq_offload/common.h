/* SPDX-License-Identifier: MIT */
/* Dual-ring shared-memory layout for the Zynq offload example. */
#ifndef ZYNQ_OFFLOAD_COMMON_H
#define ZYNQ_OFFLOAD_COMMON_H

#include "rb.h"

#include <stdint.h>
#include <stddef.h>

/*
 * Shared region is sized for the maxima below.  Actual capacity / slots /
 * slot_size are chosen at runtime via CLI and written into zo_runtime_cfg
 * so the FPGA stub uses the same geometry (same pattern as rb_config_t).
 */
#define ZO_MAX_CAPACITY   256u
#define ZO_MAX_SLOTS      256u
#define ZO_MAX_SLOT_SIZE  8192u

#define ZO_DEFAULT_CAPACITY   32u
#define ZO_DEFAULT_SLOTS      32u
#define ZO_DEFAULT_SLOT_SIZE  512u
#define ZO_DEFAULT_PAYLOAD    64u

#define ZO_SHM_NAME  "/rb_zynq_offload"
#define ZO_CFG_MAGIC 0x5A4F4346u  /* 'ZOCF' */

struct zo_result {
    uint32_t seq;
    uint32_t hash;
    uint32_t in_len;
    char     tag[4];   /* "HASH" */
    uint64_t compute_ns; /* FPGA-side processing time */
};

struct zo_doorbells {
    volatile uint32_t cpu_to_fpga;
    volatile uint32_t fpga_to_cpu;
    volatile uint32_t seq_in;
    volatile uint32_t seq_out;
};

/* Host fills this before setting ready; stub waits for ready. */
struct zo_runtime_cfg {
    volatile uint32_t magic;
    volatile uint32_t capacity;
    volatile uint32_t slots;
    volatile uint32_t slot_size;
    volatile uint32_t ready;
};

struct zo_shared {
    struct zo_doorbells   bell;
    struct zo_runtime_cfg cfg;
    uint8_t _pad0[64 - ((sizeof(struct zo_doorbells) +
                         sizeof(struct zo_runtime_cfg)) % 64)];
    uint8_t ring_a_mem[8192] __attribute__((aligned(64)));
    uint8_t ring_b_mem[8192] __attribute__((aligned(64)));
    uint8_t scratch_a[(size_t)ZO_MAX_SLOTS * ZO_MAX_SLOT_SIZE]
        __attribute__((aligned(64)));
    uint8_t scratch_b[(size_t)ZO_MAX_SLOTS * ZO_MAX_SLOT_SIZE]
        __attribute__((aligned(64)));
};

static inline rb_t *zo_ring_a(struct zo_shared *s)
{
    return (rb_t *)s->ring_a_mem;
}

static inline rb_t *zo_ring_b(struct zo_shared *s)
{
    return (rb_t *)s->ring_b_mem;
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

#endif /* ZYNQ_OFFLOAD_COMMON_H */
