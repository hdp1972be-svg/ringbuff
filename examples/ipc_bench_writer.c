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

    int fd = shm_open(RB_SHM_NAME, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            fprintf(stderr,
                    "bench_writer: %s already exists. remove it with:\n"
                    "        rm /dev/shm%s\n",
                    RB_SHM_NAME, RB_SHM_NAME);
        } else {
            perror("bench_writer: shm_open");
        }
        return 1;
    }

    size_t sz = ipc_region_size();
    if (ftruncate(fd, (off_t)sz) != 0) {
        perror("bench_writer: ftruncate");
        close(fd);
        shm_unlink(RB_SHM_NAME);
        return 1;
    }

    void *base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("bench_writer: mmap");
        close(fd);
        shm_unlink(RB_SHM_NAME);
        return 1;
    }
    close(fd);

    rb_t *rb = ipc_ring(base);
    void *scratch = ipc_scratch(base);

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = RB_CAPACITY;
    cfg.slots     = RB_SLOTS;
    cfg.slot_size = RB_SLOT_SIZE;

    rb_err_t e = rb_init(rb, &cfg, scratch, (size_t)RB_SLOTS * RB_SLOT_SIZE);
    if (e != RB_OK) {
        fprintf(stderr, "bench_writer: rb_init failed: %d\n", (int)e);
        munmap(base, sz);
        shm_unlink(RB_SHM_NAME);
        return 1;
    }

    printf("bench_writer: pid=%d shm=%s size=%zu B msg=%u B\n",
           (int)getpid(), RB_SHM_NAME, sz, (unsigned)RB_BENCH_MSG_SIZE);
    printf("bench_writer: running %.1f s at full speed (no pacing), "
           "start ipc_bench_reader now, Ctrl-C to stop early\n", seconds);
    fflush(stdout);

    bench_msg_t msg;
    memset(&msg, 0xA5, sizeof msg);

    uint64_t seq = 0;
    uint64_t full_waits = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!g_stop && elapsed_seconds(&start) < seconds) {
        msg.seq = seq;

        uint32_t idx, cap;
        void *w;
        e = rb_acquire(rb, (uint32_t)sizeof msg, &idx, &w, &cap);
        if (e == RB_ERR_FULL) {
            full_waits++;
            sched_yield();
            continue;
        }
        if (e != RB_OK) {
            fprintf(stderr, "bench_writer: acquire error %d\n", (int)e);
            break;
        }

        memcpy(w, &msg, sizeof msg);
        rb_publish(rb, idx, (uint32_t)sizeof msg);
        seq++;

        if ((seq % 5000000u) == 0u) {
            printf("bench_writer: %llu published (full_waits=%llu)\n",
                   (unsigned long long)seq,
                   (unsigned long long)full_waits);
            fflush(stdout);
        }
    }

    double s = elapsed_seconds(&start);
    double msg_s = s > 0.0 ? (double)seq / s : 0.0;
    double mb_s = msg_s * (double)sizeof msg / 1e6;
    printf("bench_writer: done. %llu published, %llu backpressure waits, "
           "target %.1f s, measured %.3f s, %.0f msg/s, %.2f MB/s\n",
           (unsigned long long)seq,
           (unsigned long long)full_waits,
           seconds,
           s,
           msg_s,
           mb_s);
    fflush(stdout);

    rb_deinit(rb);
    munmap(base, sz);
    return 0;
}