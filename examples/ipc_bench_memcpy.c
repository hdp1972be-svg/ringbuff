#define _POSIX_C_SOURCE 200809L

#ifndef RB_BENCH_MSG_SIZE
#  define RB_BENCH_MSG_SIZE 256u
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

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
            "Usage: %s -t <seconds> [-s <bytes>]\n"
            "  -t <seconds>  benchmark duration, required (no default)\n"
            "  -s <bytes>    byte count, power of two in [4, 67108864] (default %u)\n",
            prog, (unsigned)RB_BENCH_MSG_SIZE);
}

int main(int argc, char **argv) {
    double seconds = -1.0;
    unsigned msg_size = (unsigned)RB_BENCH_MSG_SIZE;
    int c;
    while ((c = getopt(argc, argv, "t:s:")) != -1) {
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
        case 's': {
            char *end = NULL;
            unsigned long v = strtoul(optarg, &end, 0);
            if (!end || *end != '\0' || v < 4ul || v > 67108864ul || (v & (v - 1ul)) != 0ul) {
                fprintf(stderr, "%s: invalid -s value '%s' (power of two in [4, 67108864])\n",
                        argv[0], optarg);
                return 2;
            }
            msg_size = (unsigned)v;
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

    uint8_t *a = malloc(msg_size);
    uint8_t *b = malloc(msg_size);
    if (!a || !b) {
        perror("bench_memcpy: malloc");
        return 1;
    }
    memset(a, 0x5A, msg_size);
    memset(b, 0xA5, msg_size);

    printf("bench_memcpy: size=%u B, hot source and destination, "
           "running %.1f s, Ctrl-C to stop early\n", msg_size, seconds);
    fflush(stdout);

    uint64_t copies = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!g_stop && elapsed_seconds(&start) < seconds) {
        memcpy(b, a, msg_size);
        if ((copies & 1023u) == 0u) {
            unsigned char v = (unsigned char)(copies >> 10);
            a[0] ^= v;
            b[0] ^= v;
        }
        uint8_t *t = a;
        a = b;
        b = t;
        copies++;
    }

    double s = elapsed_seconds(&start);
    double msg_s = s > 0.0 ? (double)copies / s : 0.0;
    double mb_s = msg_s * (double)msg_size / 1e6;
    printf("bench_memcpy: done. %llu copies, "
           "target %.1f s, measured %.3f s, %.0f msg/s, %.2f MB/s\n",
           (unsigned long long)copies,
           seconds,
           s,
           msg_s,
           mb_s);
    fflush(stdout);

    free(a);
    free(b);
    return 0;
}