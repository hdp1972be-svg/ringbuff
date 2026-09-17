/* SPDX-License-Identifier: MIT */
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
 *
 * Usage:
 *   ./zynq_fpga_stub [-t <seconds>]
 *   -t <seconds>  run for this wall-clock duration (recommended)
 *                 without -t: exit after ~2 s of idle ingress
 *
 * Typical pair:
 *   terminal 1: ./zynq_fpga_stub -t 70
 *   terminal 2: ./zynq_cpu_host  -t 60
 */
#define _POSIX_C_SOURCE 200809L

#include "hw_port.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct zo_shared *g_zo;

static double elapsed_seconds(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) +
           (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void sleep_us(unsigned us)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(us / 1000000u),
        .tv_nsec = (long)((us % 1000000u) * 1000u),
    };
    nanosleep(&ts, NULL);
}

static struct zo_shared *map_shared(void)
{
    /* Wait until cpu_host has created the segment. */
    for (int i = 0; i < 50; i++) {
        int fd = shm_open(ZO_SHM_NAME, O_RDWR, 0666);
        if (fd >= 0) {
            void *p = mmap(NULL, sizeof(struct zo_shared),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (p != MAP_FAILED)
                return (struct zo_shared *)p;
        }
        sleep_us(100000);
    }
    fprintf(stderr, "fpga_stub: timed out waiting for %s\n", ZO_SHM_NAME);
    return NULL;
}

/*
 * Process one ingress frame.
 * Returns:
 *   1  processed
 *   0  ingress empty
 *  -1  hard error
 *   2  timed out waiting for egress space (CPU not draining)
 */
static int process_one(rb_t *in, rb_t *out,
                       const struct timespec *start, double max_seconds)
{
    uint32_t idx = 0, len = 0;
    const void *obj = NULL;
    bool trunc = false;

    if (rb_consume(in, &idx, &obj, &len, &trunc) != RB_OK)
        return 0; /* empty */

    /* RB_HW_INVALIDATE_SLOT already ran inside rb_consume. */

    uint32_t h = zo_fast_hash(obj, len);
    uint32_t seq = ++g_zo->bell.seq_out;

    printf("FPGA consumed ingress slot %u len=%u trunc=%d seq=%u hash=0x%08x\n",
           idx, len, (int)trunc, seq, h);
    if (len > 0) {
        uint32_t plen = len;
        const char *s = (const char *)obj;
        for (uint32_t i = 0; i < len; i++) {
            if (s[i] == '\0') {
                plen = i;
                break;
            }
        }
        printf("FPGA json (%u B): %.*s\n", plen, (int)plen, s);
    }

    /* Publish result into egress ring B — must not block forever. */
    uint32_t oidx = 0, ocap = 0;
    void *w = NULL;
    int full_spins = 0;
    for (;;) {
        if (max_seconds > 0.0 && elapsed_seconds(start) >= max_seconds) {
            fprintf(stderr, "FPGA: duration expired while waiting for egress space\n");
            rb_release(in, idx);
            return 2;
        }

        rb_err_t e = rb_acquire(out, sizeof(struct zo_result), &oidx, &w, &ocap);
        if (e == RB_OK)
            break;
        if (e == RB_ERR_FULL) {
            if ((++full_spins % 1000) == 0) {
                fprintf(stderr,
                        "FPGA: egress ring full (CPU not draining?) — waiting…\n");
            }
            sleep_us(100);
            continue;
        }
        fprintf(stderr, "FPGA rb_acquire(egress) failed (%d)\n", (int)e);
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

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-t <seconds>]\n"
            "  -t <seconds>  wall-clock runtime (recommended for continuous runs)\n"
            "                without -t: exit after ~2 s of idle ingress\n"
            "\n"
            "Pair with: ./zynq_cpu_host -t <seconds>\n",
            prog);
}

int main(int argc, char **argv)
{
    double max_seconds = -1.0; /* <0 → idle-based exit only */
    int c;
    while ((c = getopt(argc, argv, "t:h")) != -1) {
        switch (c) {
        case 't': {
            char *end = NULL;
            max_seconds = strtod(optarg, &end);
            if (!end || *end != '\0' || !(max_seconds > 0.0)) {
                fprintf(stderr, "%s: invalid -t value '%s'\n", argv[0], optarg);
                return 2;
            }
            break;
        }
        case 'h':
        default:
            usage(argv[0]);
            return c == 'h' ? 0 : 2;
        }
    }

    printf("zynq_offload fpga_stub — waiting for shared region");
    if (max_seconds > 0.0)
        printf(" (run for %.2fs)", max_seconds);
    else
        printf(" (idle-exit after ~2s)");
    printf("\n");

    g_zo = map_shared();
    if (!g_zo)
        return 1;

    rb_t *in  = zo_ring_a(g_zo);
    rb_t *out = zo_ring_b(g_zo);

    printf("FPGA stub ready — processing continuously");
    if (max_seconds > 0.0)
        printf(" for %.2fs", max_seconds);
    printf("\n");

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int total = 0;
    int idle_rounds = 0;
    const int idle_limit = 100; /* ~2 s only used when no -t */

    for (;;) {
        if (max_seconds > 0.0 && elapsed_seconds(&start) >= max_seconds)
            break;

        int n = process_one(in, out, &start, max_seconds);
        if (n == 1) {
            total += 1;
            idle_rounds = 0;
            g_zo->bell.cpu_to_fpga = 0; /* clear doorbell */
        } else if (n == 0) {
            idle_rounds++;
            /* Only exit on idle when the user did not pass -t. */
            if (max_seconds < 0.0 && idle_rounds >= idle_limit)
                break;
            sleep_us(20000);
        } else if (n == 2) {
            /* Duration expired while blocked on egress — normal end. */
            break;
        } else {
            return 2;
        }
    }

    printf("FPGA stub done, processed %d frames (%.2fs elapsed)\n",
           total, elapsed_seconds(&start));
    return 0;
}
