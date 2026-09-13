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

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sleep_ms(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void sleep_us(long us) {
    struct timespec ts = { .tv_sec = us / 1000000,
                           .tv_nsec = (us % 1000000) * 1000L };
    nanosleep(&ts, NULL);
}

static void usage(const char *prog) {
    printf(
        "usage: %s [options]\n"
        "  -n N     total messages to publish (default 1000000)\n"
        "  -b N     burst size, items published back-to-back (default 256)\n"
        "  -q MS    quiet period between bursts, ms (default 20)\n"
        "  -m MODE  wait | drop  (default wait)\n"
        "             wait = block on full, retry\n"
        "             drop = count as dropped, move to next item\n"
        "  -v       verbose per-second output\n"
        "  -h       this help\n",
        prog);
}

int main(int argc, char **argv) {
    uint64_t total      = 1000000ull;
    uint32_t burst_size = 256u;
    long     quiet_ms   = 20;
    int      drop_mode  = 0;
    int      verbose    = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "-n") && i + 1 < argc) total      = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) burst_size = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-q") && i + 1 < argc) quiet_ms   = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-v"))                 verbose    = 1;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            ++i;
            if      (!strcmp(argv[i], "drop")) drop_mode = 1;
            else if (!strcmp(argv[i], "wait")) drop_mode = 0;
            else { fprintf(stderr, "unknown mode: %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    signal(SIGINT, on_sigint);

    int fd = shm_open(RB_SHM_NAME, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        fprintf(stderr,
                "writer: cannot create %s: %s\n"
                "        remove stale segment with: rm /dev/shm%s\n",
                RB_SHM_NAME, strerror(errno), RB_SHM_NAME);
        return 1;
    }

    size_t sz = ipc_region_size();
    if (ftruncate(fd, (off_t)sz) != 0) { perror("ftruncate"); shm_unlink(RB_SHM_NAME); return 1; }

    void *base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); shm_unlink(RB_SHM_NAME); return 1; }
    close(fd);

    rb_t *rb = ipc_ring(base);
    void *scratch = ipc_scratch(base);

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = RB_CAPACITY;
    cfg.slots     = RB_SLOTS;
    cfg.slot_size = RB_SLOT_SIZE;

    if (rb_init(rb, &cfg, scratch, (size_t)RB_SLOTS * RB_SLOT_SIZE) != RB_OK) {
        fprintf(stderr, "writer: rb_init failed\n");
        munmap(base, sz);
        shm_unlink(RB_SHM_NAME);
        return 1;
    }

    printf("writer: pid=%d mode=%s burst=%u quiet=%ldms total=%llu\n",
           (int)getpid(), drop_mode ? "drop" : "wait",
           burst_size, quiet_ms, (unsigned long long)total);
    printf("writer: Ctrl-C to stop early\n");

    double t0 = now_sec();
    double t_last_report = t0;

    uint64_t seq          = 0;   /* next message sequence number */
    uint64_t bursts_done  = 0;
    uint64_t full_waits   = 0;
    uint64_t dropped      = 0;

    while (!g_stop && seq < total) {
        /* ---- one burst ---- */
        for (uint32_t i = 0; i < burst_size && seq < total && !g_stop; ++i) {
            char msg[128];
            int len = snprintf(msg, sizeof msg,
                               "msg #%llu burst=%llu off=%u",
                               (unsigned long long)seq,
                               (unsigned long long)bursts_done,
                               i);
            if (len <= 0) break;

            uint32_t idx, cap;
            void *w;
            rb_err_t e = rb_acquire(rb, (uint32_t)len, &idx, &w, &cap);

            if (e == RB_ERR_FULL && !drop_mode) {
                /* WAIT policy: block and retry until a slot frees up. */
                while (e == RB_ERR_FULL && !g_stop) {
                    full_waits++;
                    sleep_us(100);
                    e = rb_acquire(rb, (uint32_t)len, &idx, &w, &cap);
                }
            }

            if (e == RB_ERR_FULL) {
                /* DROP policy: skip this message. Reader sees a gap. */
                dropped++;
                seq++;      /* keep seq advancing so gaps are visible */
                continue;
            }
            if (e != RB_OK) {
                fprintf(stderr, "writer: acquire err %d\n", (int)e);
                goto done;
            }

            memcpy(w, msg, (size_t)len);
            rb_publish(rb, idx, (uint32_t)len);
            seq++;
        }

        bursts_done++;

        /* ---- periodic status ---- */
        double t = now_sec();
        if (verbose && t - t_last_report >= 1.0) {
            uint32_t depth = rb_count(rb);
            printf("writer: t=%6.1fs bursts=%llu published=%llu "
                   "full_waits=%llu dropped=%llu queue=%u/%u\n",
                   t - t0,
                   (unsigned long long)bursts_done,
                   (unsigned long long)seq,
                   (unsigned long long)full_waits,
                   (unsigned long long)dropped,
                   depth, rb_limit(rb));
            t_last_report = t;
        }

        /* ---- quiet period ---- */
        if (quiet_ms > 0 && !g_stop && seq < total) sleep_ms(quiet_ms);
    }

done:;
    double t = now_sec();
    uint32_t depth = rb_count(rb);

    printf("writer: done t=%.2fs seq=%llu bursts=%llu full_waits=%llu "
           "dropped=%llu queue=%u/%u\n",
           t - t0,
           (unsigned long long)seq,
           (unsigned long long)bursts_done,
           (unsigned long long)full_waits,
           (unsigned long long)dropped,
           depth, rb_limit(rb));

#if RB_ENABLE_STATS
    const rb_stats_t *st = rb_stats(rb);
    printf("writer: stats: published=%llu high_water=%u full_hits=%u\n",
           (unsigned long long)st->published,
           st->high_water, st->full_hits);
#endif

    rb_deinit(rb);
    munmap(base, sz);
    return 0;
}
