/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/*
 * Software model of the FPGA side.
 *
 * On a real Zynq-7000 / Antminer S9 this logic lives in the PL:
 *   - AXI master reads slots from the ingress scratchpad,
 *   - computes hash / encrypt / zip,
 *   - writes results into the egress scratchpad,
 *   - advances the two rings (or asks a tiny soft-core / IRQ handler
 *     to call rb_release / rb_publish on its behalf).
 *
 * Here a normal Linux process plays the same role so the example can
 * be tested without hardware.  The ownership protocol and the RB_HW_*
 * hooks are identical.
 */
#include "hw_port.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct zo_shared *g_zo;

static struct zo_shared *map_shared(void)
{
    /* Wait until cpu_host has created the segment. */
    for (int i = 0; i < 100; i++) {
        int fd = shm_open(ZO_SHM_NAME, O_RDWR, 0666);
        if (fd >= 0) {
            void *p = mmap(NULL, sizeof(struct zo_shared),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (p != MAP_FAILED)
                return (struct zo_shared *)p;
        }
        usleep(100000);
    }
    fprintf(stderr, "fpga_stub: timed out waiting for %s\n", ZO_SHM_NAME);
    return NULL;
}

static int process_one(rb_t *in, rb_t *out, int quiet)
{
    uint32_t idx = 0, len = 0;
    const void *obj = NULL;
    bool trunc = false;

    if (rb_consume(in, &idx, &obj, &len, &trunc) != RB_OK)
        return 0; /* empty */

    /* RB_HW_INVALIDATE_SLOT already ran inside rb_consume. */

    uint32_t h = zo_fast_hash(obj, len);
    uint32_t seq = ++g_zo->bell.seq_out;

    if (!quiet) {
        printf("FPGA consumed ingress slot %u len=%u → hash=0x%08x seq=%u\n",
               idx, len, h, seq);
    }

    /* Publish result into egress ring B. */
    uint32_t oidx = 0, ocap = 0;
    void *w = NULL;
    for (;;) {
        rb_err_t e = rb_acquire(out, sizeof(struct zo_result), &oidx, &w, &ocap);
        if (e == RB_OK)
            break;
        if (e == RB_ERR_FULL) {
            usleep(20);
            continue;
        }
        fprintf(stderr, "FPGA rb_acquire(egress) failed\n");
        rb_release(in, idx);
        return -1;
    }

    struct zo_result *r = (struct zo_result *)w;
    r->seq    = seq;
    r->hash   = h;
    r->in_len = len;
    memcpy(r->tag, "HASH", 4);

    if (rb_publish(out, oidx, (uint32_t)sizeof *r) != RB_OK) {
        fprintf(stderr, "FPGA rb_publish(egress) failed\n");
        rb_abort(out);
        rb_release(in, idx);
        return -1;
    }
    /* RB_HW_FLUSH_SLOT + RB_HW_NOTIFY_DEVICE already ran.
     * Also raise the CPU-visible doorbell (stand-in for IRQ). */
    g_zo->bell.fpga_to_cpu = 1u;

    rb_release(in, idx);
    return 1;
}

int main(int argc, char **argv)
{
    int quiet = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-q"))
            quiet = 1;
    }

    printf("zynq_offload fpga_stub — waiting for shared region\n");
    g_zo = map_shared();
    if (!g_zo)
        return 1;

    rb_t *in  = zo_ring_a(g_zo);
    rb_t *out = zo_ring_b(g_zo);

    printf("FPGA stub ready — processing until idle for a while\n");

    int total = 0;
    int idle_rounds = 0;
    /* Longer idle tolerance for stress runs that pause between bursts. */
    while (idle_rounds < 500) {          /* ~5 s of idle → exit */
        int n = process_one(in, out, quiet || total > 20);
        if (n > 0) {
            total += n;
            idle_rounds = 0;
            g_zo->bell.cpu_to_fpga = 0; /* clear doorbell */
            if (total % 1000 == 0 && quiet)
                printf("FPGA processed %d frames so far\n", total);
        } else if (n == 0) {
            idle_rounds++;
            usleep(10000);
        } else {
            return 2;
        }
    }

    printf("FPGA stub done, processed %d frames\n", total);
    return 0;
}
