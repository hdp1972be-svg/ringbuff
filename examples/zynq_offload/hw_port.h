/* SPDX-License-Identifier: MIT */
/*
 * Platform overrides for the three hardware-visibility stubs.
 *
 * On a real Antminer S9 / Zynq-7000 you would:
 *   - replace the compiler barriers with dsb(sy) / dmb(sy) or the
 *     Xilinx cache-maintenance helpers,
 *   - write an AXI-lite doorbell register instead of the shared flag,
 *   - optionally raise an MSI / GIC interrupt.
 *
 * For the host-testable stub we only need compiler barriers so that
 * the two processes (cpu_host and fpga_stub) see each other's stores
 * in the POSIX shared-memory region.
 */
#ifndef ZYNQ_OFFLOAD_HW_PORT_H
#define ZYNQ_OFFLOAD_HW_PORT_H

#include "common.h"

/* The shared region is set by the process that maps it. */
extern struct zo_shared *g_zo;

/* ---- producer side: make CPU writes visible, then ring doorbell ---- */

static inline void zo_flush_slot(rb_t *rb, uint32_t idx)
{
    (void)rb;
    (void)idx;
    /* On Zynq:
     *   Xil_DCacheFlushRange((UINTPTR)rb_slot_ptr(rb, idx), len);
     *   dsb(sy);
     * On host test: compiler barrier is enough for MAP_SHARED. */
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("" ::: "memory");
#endif
}

static inline void zo_notify_device(rb_t *rb, uint32_t idx,
                                    uint32_t len, bool trunc)
{
    (void)rb;
    (void)idx;
    (void)len;
    (void)trunc;
    if (!g_zo)
        return;
    /* On the board: writel(1, &fpga_regs->doorbell_in); */
    g_zo->bell.cpu_to_fpga = 1u;
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("" ::: "memory");
#endif
}

/* ---- consumer side: see device writes before reading the payload ---- */

static inline void zo_invalidate_slot(rb_t *rb, uint32_t idx)
{
    (void)rb;
    (void)idx;
    /* On Zynq:
     *   Xil_DCacheInvalidateRange((UINTPTR)rb_slot_ptr(rb, idx), len);
     *   dsb(sy);
     */
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("" ::: "memory");
#endif
}

/* Install the overrides for this translation unit. */
#undef  RB_HW_FLUSH_SLOT
#define RB_HW_FLUSH_SLOT(rb, idx)           zo_flush_slot((rb), (idx))

#undef  RB_HW_INVALIDATE_SLOT
#define RB_HW_INVALIDATE_SLOT(rb, idx)      zo_invalidate_slot((rb), (idx))

#undef  RB_HW_NOTIFY_DEVICE
#define RB_HW_NOTIFY_DEVICE(rb, idx, len, trunc) \
    zo_notify_device((rb), (idx), (len), (trunc))

#endif /* ZYNQ_OFFLOAD_HW_PORT_H */
