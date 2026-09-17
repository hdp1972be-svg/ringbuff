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
#include <sys/stat.h>

/* Cap stored latency samples so a multi-minute run cannot exhaust RAM.
 * 16M × 8 B ≈ 128 MiB. Beyond the cap we keep min/max/avg but stop
 * collecting for percentiles. */
#define LAT_CAP (16u * 1024u * 1024u)

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static double elapsed_seconds(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) +
           (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile. samples must be sorted ascending, n >= 1.
 * p is in (0, 100]; returns samples[round((p/100)*(n-1))]. */
static uint64_t percentile(const uint64_t *samples, size_t n, double p) {
    if (n == 0)
        return 0;
    if (n == 1)
        return samples[0];
    double rank = (p / 100.0) * (double)(n - 1);
    size_t i = (size_t)(rank + 0.5);
    if (i >= n)
        i = n - 1;
    return samples[i];
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s -t <seconds> [-s <bytes>]\n"
            "  -t <seconds>  benchmark duration, required (no default)\n"
            "  -s <bytes>    message size, power of two in [4, 4096] (default %u)\n",
            prog, (unsigned)RB_BENCH_MSG_SIZE);
}

int main(int argc, char **argv) {
    double seconds = -1.0;
    unsigned msg_size = (unsigned)RB_BENCH_MSG_SIZE;
    int c;
    while ((c = getopt(argc, argv, "t:s:")) != -1) {
        switch (c) {
        case 's': {
            char *end = NULL;
            unsigned long v = strtoul(optarg, &end, 0);
            if (!end || *end != '\0' || v < 4u || v > 4096u ||
                (v & (v - 1u)) != 0u) {
                fprintf(stderr, "%s: invalid -s value '%s' (power of two in [4, 4096])\n",
                        argv[0], optarg);
                return 2;
            }
            msg_size = (unsigned)v;
            break;
        }
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

    struct timespec proc_start;
    clock_gettime(CLOCK_MONOTONIC, &proc_start);

    int fd = -1;
    size_t sz = 0;
    void *base = MAP_FAILED;
    for (;;) {
        struct stat st;
        fd = shm_open(RB_SHM_NAME, O_RDWR, 0);
        if (fd >= 0) {
            if (fstat(fd, &st) != 0) {
                perror("bench_reader: fstat");
                close(fd);
                return 1;
            }
            sz = (size_t)st.st_size;
            if (sz > 0) {
                base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (base != MAP_FAILED) {
                    close(fd);
                    break;
                }
                perror("bench_reader: mmap");
                close(fd);
                return 1;
            }
            close(fd);
        } else if (errno != ENOENT) {
            perror("bench_reader: shm_open");
            return 1;
        }
        if (elapsed_seconds(&proc_start) >= seconds + 2.0) {
            fprintf(stderr,
                    "bench_reader: %s not ready after %.1fs "
                    "(start ipc_bench_writer first)\n",
                    RB_SHM_NAME, seconds + 2.0);
            return 1;
        }
        struct timespec ts = {0, 1 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    rb_t *rb = ipc_ring(base);

    uint64_t received = 0, bytes = 0, gaps = 0, truncations = 0;
    uint64_t empty_spins = 0, drain_empties = 0;
    uint64_t last_seq = 0;
    uint64_t lat_sum = 0, lat_count = 0, lat_min = UINT64_MAX, lat_max = 0;

    /* Growable sample buffer for percentile calculation. */
    uint64_t *lat_samples = NULL;
    size_t lat_n = 0, lat_cap = 0;
    int lat_capped = 0;

    struct timespec window_start;
    int have_window = 0;

    printf("bench_reader: pid=%d shm=%s size=%zu B msg=%u B\n",
           (int)getpid(), RB_SHM_NAME, sz, msg_size);
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
            if (have_window) {
                break;
            }
            if (elapsed_seconds(&proc_start) >= seconds + 2.0) {
                break;
            }
            sched_yield();
            continue;
        }
        if (r != RB_OK) {
            fprintf(stderr, "bench_reader: consume error %d\n", (int)r);
            break;
        }
        drain_empties = 0;

        if (trunc || szmsg < 4u) {
            truncations++;
        } else {
            uint32_t seq;
            memcpy(&seq, w, sizeof seq);
            if (!have_window) {
                have_window = 1;
                clock_gettime(CLOCK_MONOTONIC, &window_start);
            } else if ((uint32_t)((uint64_t)seq - last_seq) != 1u) {
                gaps++;
            }
            last_seq = seq;
            if (msg_size >= 12u && szmsg >= 12u) {
                uint64_t ts;
                struct timespec now;
                memcpy(&ts, (const uint8_t *)w + 4, sizeof ts);
                clock_gettime(CLOCK_MONOTONIC, &now);
                uint64_t d = (uint64_t)now.tv_sec * 1000000000ull +
                             (uint64_t)now.tv_nsec - ts;
                lat_sum += d;
                lat_count++;
                if (d < lat_min) {
                    lat_min = d;
                }
                if (d > lat_max) {
                    lat_max = d;
                }
                /* Collect sample for percentiles (until LAT_CAP). */
                if (!lat_capped) {
                    if (lat_n >= lat_cap) {
                        size_t nc = lat_cap ? lat_cap * 2u : 65536u;
                        if (nc > LAT_CAP)
                            nc = LAT_CAP;
                        uint64_t *nbuf = (uint64_t *)realloc(lat_samples,
                                                             nc * sizeof(uint64_t));
                        if (!nbuf) {
                            /* OOM — keep running without further samples. */
                            lat_capped = 1;
                        } else {
                            lat_samples = nbuf;
                            lat_cap = nc;
                        }
                    }
                    if (!lat_capped && lat_n < lat_cap) {
                        lat_samples[lat_n++] = d;
                        if (lat_n >= LAT_CAP)
                            lat_capped = 1;
                    }
                }
            }
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
        free(lat_samples);
        munmap(base, sz);
        return 1;
    }

    double s = elapsed_seconds(&window_start);
    double msg_s = s > 0.0 ? (double)received / s : 0.0;
    double mb_s = msg_s * (double)msg_size / 1e6;
    char lat[256];
    if (msg_size >= 12u && lat_count > 0u) {
        if (lat_n > 0) {
            qsort(lat_samples, lat_n, sizeof(uint64_t), cmp_u64);
            uint64_t p10  = percentile(lat_samples, lat_n, 10.0);
            uint64_t p50  = percentile(lat_samples, lat_n, 50.0);
            uint64_t p90  = percentile(lat_samples, lat_n, 90.0);
            uint64_t p99  = percentile(lat_samples, lat_n, 99.0);
            uint64_t p999 = percentile(lat_samples, lat_n, 99.9);
            snprintf(lat, sizeof lat,
                     ", latency avg %llu min %llu max %llu ns"
                     " | p10 %llu p50 %llu p90 %llu p99 %llu p99.9 %llu ns"
                     "%s",
                     (unsigned long long)(lat_sum / lat_count),
                     (unsigned long long)lat_min,
                     (unsigned long long)lat_max,
                     (unsigned long long)p10,
                     (unsigned long long)p50,
                     (unsigned long long)p90,
                     (unsigned long long)p99,
                     (unsigned long long)p999,
                     lat_capped ? " (samples capped)" : "");
        } else {
            snprintf(lat, sizeof lat, ", latency avg %llu min %llu max %llu ns",
                     (unsigned long long)(lat_sum / lat_count),
                     (unsigned long long)lat_min,
                     (unsigned long long)lat_max);
        }
    } else {
        snprintf(lat, sizeof lat, ", latency n/a ns");
    }
    printf("bench_reader: done. %llu received, %llu bytes, %llu gaps, "
           "%llu trims, %llu empty_spins, target %.1f s, "
           "measured %.3f s, %.0f msg/s, %.2f MB/s%s\n",
           (unsigned long long)received,
           (unsigned long long)bytes,
           (unsigned long long)gaps,
           (unsigned long long)truncations,
           (unsigned long long)empty_spins,
           seconds,
           s,
           msg_s,
           mb_s,
           lat);
    fflush(stdout);

    free(lat_samples);
    munmap(base, sz);
    return 0;
}
