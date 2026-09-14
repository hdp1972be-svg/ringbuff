#define _POSIX_C_SOURCE 200809L

#define RB_SHM_NAME "/rb_ipc_bench"
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
#include <sched.h>
#include <sys/mman.h>

typedef struct {
    uint64_t seq;
    uint8_t data[RB_BENCH_MSG_SIZE - sizeof(uint64_t)];
} bench_msg_t;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static double elapsed_seconds(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) +
           (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s -t <seconds>\n"
            "  -t <seconds>  benchmark duration, required (no default)\n",
            prog);
}

int main(int argc, char **argv) {
    double seconds = -1.0;
    int c;
    while ((c = getopt(argc, argv, "t:")) != -1) {
        switch (c) {
        case 't': {
            char *end = NULL;
            seconds = strtod(optarg, &end);
            if (!end || *end != '\0' || !(seconds > 0.0)) {
                fprintf(stderr, "%s: invalid -t value '%s'\n", argv[0], optarg);
                return 2;
            }
            break;
        }
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (!(seconds > 0.0)) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, on_sigint);

    int fd = shm_open(RB_SHM_NAME, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "bench_reader: shm_open %s (start ipc_bench_writer first)\n",
                RB_SHM_NAME);
        perror("bench_reader: shm_open");
        return 1;
    }

    size_t sz = ipc_region_size();
    void *base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("bench_reader: mmap");
        close(fd);
        return 1;
    }
    close(fd);

    rb_t *rb = ipc_ring(base);

    uint64_t received = 0, bytes = 0, gaps = 0, truncations = 0;
    uint64_t empty_spins = 0, drain_empties = 0;
    uint64_t last_seq = 0;
    struct timespec window_start;
    int have_window = 0;

    printf("bench_reader: pid=%d shm=%s size=%zu B\n",
           (int)getpid(), RB_SHM_NAME, sz);
    printf("bench_reader: waiting for first message ...\n");
    fflush(stdout);

    for (;;) {
        if (g_stop) {
            break;
        }

        uint32_t idx, szmsg;
        const void *w;
        bool trunc;
        rb_err_t r = rb_consume(rb, &idx, &w, &szmsg, &trunc);
        if (r == RB_ERR_EMPTY) {
            empty_spins++;
            if (have_window) {
                drain_empties++;
                if (drain_empties >= 100000u &&
                    elapsed_seconds(&window_start) >= seconds + 1.0) {
                    break;
                }
            }
            sched_yield();
            continue;
        }
        if (r == RB_ERR_NOT_INIT) {
            break;
        }
        if (r != RB_OK) {
            fprintf(stderr, "bench_reader: consume error %d\n", (int)r);
            break;
        }
        drain_empties = 0;

        if (trunc || szmsg < sizeof(uint64_t)) {
            truncations++;
        } else {
            uint64_t seq;
            memcpy(&seq, w, sizeof seq);
            if (!have_window) {
                have_window = 1;
                clock_gettime(CLOCK_MONOTONIC, &window_start);
            } else if (seq != last_seq + 1u) {
                gaps++;
            }
            last_seq = seq;
        }
        bytes += szmsg;
        received++;

        rb_release(rb, idx);

        if ((received % 5000000u) == 0u) {
            printf("bench_reader: %llu received (gaps=%llu, empty_spins=%llu)\n",
                   (unsigned long long)received,
                   (unsigned long long)gaps,
                   (unsigned long long)empty_spins);
            fflush(stdout);
        }
    }

    if (!have_window) {
        printf("bench_reader: no messages received\n");
        munmap(base, sz);
        return 1;
    }

    double s = elapsed_seconds(&window_start);
    double msg_s = s > 0.0 ? (double)received / s : 0.0;
    double mb_s = msg_s * (double)sizeof(bench_msg_t) / 1e6;
    printf("bench_reader: done. %llu received, %llu bytes, %llu gaps, "
           "%llu trims, %llu empty_spins, target %.1f s, "
           "measured %.3f s, %.0f msg/s, %.2f MB/s\n",
           (unsigned long long)received,
           (unsigned long long)bytes,
           (unsigned long long)gaps,
           (unsigned long long)truncations,
           (unsigned long long)empty_spins,
           seconds,
           s,
           msg_s,
           mb_s);
    fflush(stdout);

    munmap(base, sz);
    return 0;
}