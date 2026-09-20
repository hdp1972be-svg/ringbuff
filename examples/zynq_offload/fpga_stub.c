/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/*
 * FPGA stub — high-rate FNV hash path.
 * Waits for host zo_runtime_cfg (capacity/slots/slot_size), then busy-spins.
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
    for (int i = 0; i < 200; i++) {
        int fd = shm_open(ZO_SHM_NAME, O_RDWR, 0666);
        if (fd >= 0) {
            void *p = mmap(NULL, sizeof(struct zo_shared),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (p != MAP_FAILED)
                return (struct zo_shared *)p;
        }
        usleep(20000);
    }
    fprintf(stderr, "fpga_stub: timed out waiting for %s\n", ZO_SHM_NAME);
    return NULL;
}

static int process_one(rb_t *in, rb_t *out)
{
    uint32_t idx = 0, len = 0;
    const void *obj = NULL;
    bool trunc = false;

    if (rb_consume(in, &idx, &obj, &len, &trunc) != RB_OK)
        return 0;

    uint32_t h = zo_fast_hash(obj, len);
    uint32_t seq = ++g_zo->bell.seq_out;

    uint32_t oidx = 0, ocap = 0;
    void *w = NULL;
    for (;;) {
        rb_err_t e = rb_acquire(out, sizeof(struct zo_result), &oidx, &w, &ocap);
        if (e == RB_OK)
            break;
        if (e == RB_ERR_FULL) {
            for (volatile int i = 0; i < 64; i++)
                ;
            continue;
        }
        rb_release(in, idx);
        return -1;
    }

    struct zo_result *r = (struct zo_result *)w;
    r->seq = seq;
    r->hash = h;
    r->in_len = len;
    memcpy(r->tag, "HASH", 4);

    if (rb_publish(out, oidx, (uint32_t)sizeof *r) != RB_OK) {
        rb_abort(out);
        rb_release(in, idx);
        return -1;
    }
    g_zo->bell.fpga_to_cpu = 1u;
    rb_release(in, idx);
    return 1;
}

int main(int argc, char **argv)
{
    int quiet = 0;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "-q"))
            quiet = 1;

    printf("zynq_offload fpga_stub — waiting for shared region\n");
    g_zo = map_shared();
    if (!g_zo)
        return 1;

    for (int i = 0; i < 10000; i++) {
        if (g_zo->cfg.ready && g_zo->cfg.magic == ZO_CFG_MAGIC)
            break;
        usleep(1000);
    }
    if (!g_zo->cfg.ready || g_zo->cfg.magic != ZO_CFG_MAGIC) {
        fprintf(stderr, "fpga_stub: host never published runtime cfg\n");
        return 1;
    }

    printf("FPGA stub ready  capacity=%u slots=%u slot_size=%u (FNV)\n",
           g_zo->cfg.capacity, g_zo->cfg.slots, g_zo->cfg.slot_size);

    rb_t *in = zo_ring_a(g_zo);
    rb_t *out = zo_ring_b(g_zo);

    int total = 0;
    int idle_spins = 0;
    const int idle_exit = 500000000;

    while (idle_spins < idle_exit) {
        if (!g_zo->cfg.ready && idle_spins > 1000000)
            break;

        int n = process_one(in, out);
        if (n > 0) {
            total += n;
            idle_spins = 0;
            g_zo->bell.cpu_to_fpga = 0;
            if (!quiet && (total % 200000) == 0)
                printf("FPGA processed %d frames\n", total);
        } else if (n == 0) {
            idle_spins++;
            if ((idle_spins & 0xfff) == 0) {
                for (volatile int i = 0; i < 32; i++)
                    ;
            }
        } else {
            return 2;
        }
    }

    printf("FPGA stub done, processed %d frames\n", total);
    return 0;
}
