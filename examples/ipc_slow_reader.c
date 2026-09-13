#define _POSIX_C_SOURCE 200809L

#include "ipc_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sleep_us(long us) {
    struct timespec ts = { .tv_sec = us / 1000000,
                           .tv_nsec = (us % 1000000) * 1000L };
    nanosleep(&ts, NULL);
}

static void usage(const char *prog) {
    printf(
        "usage: %s [options]\n"
        "  -d US    per-item processing cost, microseconds (default 100)\n"
        "  -n N     exit after N items received (0 = unlimited, default 0)\n"
        "  -t MS    idle timeout, ms (default 5000; 0 = never exit on idle)\n"
        "  -v       verbose per-second output\n"
        "  -h       this help\n",
        prog);
}

int main(int argc, char **argv) {
    long     per_item_us   = 100;
    uint64_t target        = 0;
    long     idle_timeout  = 5000;
    int      verbose       = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "-d") && i + 1 < argc) per_item_us  = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) target       = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) idle_timeout = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-v"))                 verbose      = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    signal(SIGINT, on_sigint);

    /* ---- attach. Retry until the writer creates the segment. ---- */
    size_t sz = ipc_region_size();
    void  *base = MAP_FAILED;
    int    fd   = -1;

    for (;;) {
        if (g_stop) return 0;
        fd = shm_open(RB_SHM_NAME, O_RDWR, 0);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && (size_t)st.st_size >= sz) {
                base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                close(fd);
                if (base != MAP_FAILED) break;
            } else {
                close(fd);
            }
        }
        printf("reader: waiting for writer to create %s...\n", RB_SHM_NAME);
        sleep_us(200000);
    }

    rb_t *rb = ipc_ring(base);

    /* Ring may be mmap'd before the writer has called rb_init.
       rb_capacity returns 0 until rb_init sets it. Spin until ready. */
    for (;;) {
        if (g_stop) { munmap(base, sz); return 0; }
        if (rb_capacity(rb) == RB_CAPACITY) break;
        sleep_us(10000);
    }

    printf("reader: pid=%d attached, per_item=%ldus\n", (int)getpid(), per_item_us);
    printf("reader: Ctrl-C to stop\n");

    double t0 = now_sec();
    double t_last_report = t0;
    double t_last_data = t0;

    uint64_t received        = 0;
    uint64_t last_seq        = 0;
    uint64_t first_seq_seen  = 0;
    uint64_t gaps            = 0;
    uint64_t gap_items       = 0;
    uint64_t truncations     = 0;
    uint32_t hw_depth        = 0;
    uint64_t samples_full    = 0;
    uint64_t samples_empty   = 0;
    uint64_t samples_total   = 0;

    while (!g_stop) {
        uint32_t idx, len;
        const void *obj;
        bool trunc;

        rb_err_t e = rb_consume(rb, &idx, &obj, &len, &trunc);

        if (e == RB_ERR_EMPTY) {
            uint32_t depth = rb_count(rb);
            samples_total++;
            if (depth == 0) samples_empty++;
            if (depth >= rb_limit(rb)) samples_full++;

            /* Idle timeout check. */
            if (idle_timeout > 0 && received > 0) {
                double t = now_sec();
                if ((t - t_last_data) * 1000.0 > (double)idle_timeout) {
                    printf("reader: idle for %ld ms, assuming writer is done\n",
                           idle_timeout);
                    break;
                }
            }
            sleep_us(50);
            continue;
        }
        if (e != RB_OK) {
            fprintf(stderr, "reader: consume err %d\n", (int)e);
            break;
        }

        /* ---- measure queue depth at this sample point ---- */
        uint32_t depth = rb_count(rb);
        if (depth > hw_depth) hw_depth = depth;
        samples_total++;
        if (depth == 0) samples_empty++;
        if (depth >= rb_limit(rb)) samples_full++;

        /* ---- parse and verify sequence ---- */
        if (trunc) {
            truncations++;
        } else if (len > 0) {
            unsigned long long n = 0;
            if (sscanf((const char *)obj, "msg #%llu", &n) == 1) {
                if (first_seq_seen == 0) {
                    first_seq_seen = n;
                } else if (n != last_seq + 1) {
                    /* gap: writer dropped, or we skipped */
                    uint64_t gap = (n > last_seq + 1) ? (n - last_seq - 1) : 0;
                    if (gap > 0) {
                        gaps++;
                        gap_items += gap;
                        if (gaps <= 5) {
                            fprintf(stderr,
                                    "reader: gap #%llu at seq %llu -> %llu "
                                    "(missing %llu)\n",
                                    (unsigned long long)gaps,
                                    (unsigned long long)(last_seq + 1),
                                    (unsigned long long)n,
                                    (unsigned long long)gap);
                        }
                    }
                }
                last_seq = n;
            }
        }

        rb_release(rb, idx);
        received++;
        t_last_data = now_sec();

        /* ---- simulated processing cost ---- */
        if (per_item_us > 0) sleep_us(per_item_us);

        /* ---- periodic status ---- */
        if (verbose) {
            double t = now_sec();
            if (t - t_last_report >= 1.0) {
                uint32_t d = rb_count(rb);
                double rate = (double)received / (t - t0);
                printf("reader: t=%6.1fs recv=%llu (%.0f/s) queue=%u/%u "
                       "gaps=%llu dropped=%llu\n",
                       t - t0,
                       (unsigned long long)received, rate,
                       d, rb_limit(rb),
                       (unsigned long long)gaps,
                       (unsigned long long)gap_items);
                t_last_report = t;
            }
        }

        if (target > 0 && received >= target) break;
    }

    double t = now_sec();
    printf("reader: done t=%.2fs received=%llu\n",
           t - t0, (unsigned long long)received);
    printf("reader: gaps=%llu (missing items=%llu)  truncations=%llu\n",
           (unsigned long long)gaps,
           (unsigned long long)gap_items,
           (unsigned long long)truncations);
    printf("reader: max queue depth=%u/%u  "
           "samples: full=%llu empty=%llu total=%llu\n",
           hw_depth, rb_limit(rb),
           (unsigned long long)samples_full,
           (unsigned long long)samples_empty,
           (unsigned long long)samples_total);

    rb_deinit(rb);
    munmap(base, sz);
    return 0;
}
