/* SPDX-License-Identifier: MIT */
/*
 * CPU side of the Zynq dual-ring offload example.
 *
 * Continuously generates JSON frames at the fastest rate possible for
 * -t <seconds>, publishes them into ingress ring A, and drains hashed
 * results from egress ring B.  When the ingress ring is full the packet
 * is dropped (counted) so the writer never blocks.
 *
 * Ring size is chosen at init with -s (rb_config_set_capacity/slots).
 * Quiet by default; -v enables per-packet dumps.
 *
 * Typical pair:
 *   terminal 1: ./zynq_fpga_stub -t 70
 *   terminal 2: ./zynq_cpu_host  -t 60 -s 256
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
static int g_verbose;

static void sleep_us(unsigned us)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(us / 1000000u),
        .tv_nsec = (long)((us % 1000000u) * 1000u),
    };
    nanosleep(&ts, NULL);
}

static struct zo_shared *map_shared(int create)
{
    int fd = shm_open(ZO_SHM_NAME, O_RDWR | (create ? O_CREAT : 0), 0666);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }
    if (create && ftruncate(fd, (off_t)sizeof(struct zo_shared)) != 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, sizeof(struct zo_shared),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return (struct zo_shared *)p;
}

/* Init both rings with the same capacity/slots (from -s). */
static int init_rings(struct zo_shared *s, uint32_t slots)
{
    slots = zo_clamp_slots(slots);

    /* Control blocks must fit. */
    if (rb_size(slots) > ZO_RING_MEM) {
        fprintf(stderr, "rb_size(%u)=%zu exceeds ZO_RING_MEM=%u\n",
                slots, rb_size(slots), (unsigned)ZO_RING_MEM);
        return -1;
    }

    rb_config_t cfg;
    rb_config_init(&cfg);
    rb_config_set_capacity(&cfg, slots);
    rb_config_set_slots(&cfg, slots);
    rb_config_set_slot_size(&cfg, ZO_SLOT_SIZE);
    rb_config_set_limit(&cfg, slots);

    size_t scratch_need = (size_t)slots * ZO_SLOT_SIZE;
    if (rb_init(zo_ring_a(s), &cfg, s->scratch_a, sizeof s->scratch_a) != RB_OK)
        return -1;
    if (rb_init(zo_ring_b(s), &cfg, s->scratch_b, sizeof s->scratch_b) != RB_OK)
        return -1;

    (void)scratch_need;
    s->bell.cpu_to_fpga = 0;
    s->bell.fpga_to_cpu = 0;
    s->bell.seq_in = 0;
    s->bell.seq_out = 0;
    s->bell.slots = slots;
    s->bell.ready = 1u; /* FPGA may proceed */
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("" ::: "memory");
#endif
    return 0;
}

static const char *sample_payloads[] = {
    "mining.subscribe",
    "mining.authorize",
    "mining.submit",
    "mining.notify",
    "heartbeat",
};

static int build_json(char *buf, size_t bufsz, uint64_t ts, uint32_t seq,
                      const char *payload)
{
    int n = snprintf(buf, bufsz,
                     "{\"ts\":%llu,\"seq\":%u,\"payload\":\"%s\"}",
                     (unsigned long long)ts, seq, payload);
    if (n < 0 || (size_t)n + 1u > bufsz)
        return -1;
    return n + 1;
}

static int try_publish(rb_t *ring, const char *json, uint32_t len)
{
    uint32_t idx = 0, cap = 0;
    void *w = NULL;

    rb_err_t e = rb_acquire(ring, len, &idx, &w, &cap);
    if (e == RB_ERR_FULL)
        return 1;
    if (e != RB_OK) {
        fprintf(stderr, "rb_acquire failed (%d)\n", (int)e);
        return -1;
    }

    uint32_t n = len <= cap ? len : cap;
    memcpy(w, json, n);

    if (rb_publish(ring, idx, n) != RB_OK) {
        fprintf(stderr, "rb_publish failed\n");
        return -1;
    }
    return 0;
}

static int drain_available(rb_t *ring)
{
    int got = 0;
    for (;;) {
        uint32_t idx = 0, len = 0;
        const void *obj = NULL;
        bool trunc = false;

        if (rb_consume(ring, &idx, &obj, &len, &trunc) != RB_OK) {
            if (g_zo->bell.fpga_to_cpu)
                g_zo->bell.fpga_to_cpu = 0;
            break;
        }

        if (len < sizeof(struct zo_result)) {
            fprintf(stderr, "short result len=%u\n", len);
            rb_release(ring, idx);
            continue;
        }

        if (g_verbose) {
            uint64_t ts_recv = zo_now_ns();
            const struct zo_result *r = (const struct zo_result *)obj;
            int64_t delta_ns = (int64_t)ts_recv - (int64_t)r->ts_in;

            printf(ZO_CLR_GREEN
                   "[Host R] seq=%u hash=0x%08x in_len=%u tag=%.4s%s\n"
                   "[Host R] payload=\"%.60s\"\n"
                   "[Host R] ts_in=%llu ts_fpga=%llu ts_recv=%llu delta_ns=%lld\n"
                   ZO_CLR_RESET,
                   r->seq, r->hash, r->in_len, r->tag,
                   trunc ? " (trunc)" : "",
                   r->payload,
                   (unsigned long long)r->ts_in,
                   (unsigned long long)r->ts_fpga,
                   (unsigned long long)ts_recv,
                   (long long)delta_ns);
            zo_print_sep();
        }

        rb_release(ring, idx);
        got++;
    }
    return got;
}

static int drain_until_quiet(rb_t *ring, double quiet_s, double deadline)
{
    int got = 0;
    double last_hit = zo_now_sec();
    int have_hit = 0;

    for (;;) {
        double now = zo_now_sec();
        if (now >= deadline)
            break;

        int n = drain_available(ring);
        if (n > 0) {
            got += n;
            last_hit = now;
            have_hit = 1;
            continue;
        }
        if (have_hit && (now - last_hit) >= quiet_s)
            break;
        sleep_us(200);
    }
    return got;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -t <seconds> [-s <slots>] [-v] [-h]\n"
            "\n"
            "What this program does\n"
            "  CPU (PS) side of the dual-ring Zynq offload demo. Continuously\n"
            "  generates JSON frames {ts,seq,payload} at full speed into ingress\n"
            "  ring A for -t seconds.  When the ring is full the packet is\n"
            "  dropped (counted) — the writer never blocks.  Concurrently drains\n"
            "  hashed results from egress ring B.\n"
            "\n"
            "  Pair with ./zynq_fpga_stub in another terminal.  Shared memory:\n"
            "  %s\n"
            "\n"
            "Options\n"
            "  -t <seconds>  publish duration (required, must be > 0)\n"
            "  -s <slots>    ring capacity/slots for BOTH ingress and egress\n"
            "                (default %u, max %u).  Applied at rb_init via\n"
            "                rb_config_set_capacity / rb_config_set_slots.\n"
            "                Larger values absorb bursts and reduce drops.\n"
            "  -v            verbose: per-packet dumps + periodic rate lines.\n"
            "                Default is quiet (final stats only).\n"
            "  -h            show this help and exit\n"
            "\n"
            "Example\n"
            "  terminal 1: ./zynq_fpga_stub -t 70\n"
            "  terminal 2: ./zynq_cpu_host  -t 60 -s 256\n",
            prog, ZO_SHM_NAME,
            (unsigned)ZO_DEFAULT_SLOTS, (unsigned)ZO_MAX_SLOTS);
}

int main(int argc, char **argv)
{
    double seconds = -1.0;
    uint32_t slots = ZO_DEFAULT_SLOTS;
    int c;

    while ((c = getopt(argc, argv, "t:s:vh")) != -1) {
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
            if (!end || *end != '\0' || v < 1ul) {
                fprintf(stderr, "%s: invalid -s value '%s'\n", argv[0], optarg);
                return 2;
            }
            slots = zo_clamp_slots((uint32_t)v);
            if ((unsigned long)slots != v) {
                fprintf(stderr, "%s: -s clamped to %u (max %u)\n",
                        argv[0], slots, (unsigned)ZO_MAX_SLOTS);
            }
            break;
        }
        case 'v':
            g_verbose = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (!(seconds > 0.0)) {
        usage(argv[0]);
        return 2;
    }

    printf("zynq_offload cpu_host — duration=%.2fs slots=%u verbose=%s\n",
           seconds, slots, g_verbose ? "on" : "off (stats at end only)");
    printf("hint: start ./zynq_fpga_stub [-t %.0f] in another terminal\n",
           seconds + 10.0);

    g_zo = map_shared(1);
    if (!g_zo)
        return 1;

    /* Clear ready until init completes (FPGA waits on this). */
    g_zo->bell.ready = 0;

    if (((uintptr_t)zo_ring_a(g_zo) % RB_CACHE_LINE) != 0 ||
        ((uintptr_t)zo_ring_b(g_zo) % RB_CACHE_LINE) != 0 ||
        ((uintptr_t)g_zo->scratch_a % RB_CACHE_LINE) != 0 ||
        ((uintptr_t)g_zo->scratch_b % RB_CACHE_LINE) != 0) {
        fprintf(stderr, "shared-region alignment broken (need RB_CACHE_LINE=%u)\n",
                (unsigned)RB_CACHE_LINE);
        return 1;
    }

    if (init_rings(g_zo, slots) != 0) {
        fprintf(stderr, "rb_init failed (check alignment / sizes)\n");
        return 1;
    }

    const int n_payloads = (int)(sizeof sample_payloads / sizeof sample_payloads[0]);
    if (g_verbose) {
        printf("publishing JSON into ring A for %.2fs at full speed "
               "(slots=%u, drop on full)\n", seconds, slots);
    }

    double t0 = zo_now_sec();
    double t_end = t0 + seconds;
    double t_last_report = t0;

    uint64_t published = 0;
    uint64_t dropped   = 0;
    uint64_t received  = 0;
    uint32_t seq       = 0;
    int payload_i      = 0;
    char json_buf[ZO_SLOT_SIZE];

    while (zo_now_sec() < t_end) {
        uint32_t this_seq = ++seq;
        uint64_t ts = zo_now_ns();
        const char *pl = sample_payloads[payload_i % n_payloads];
        payload_i++;

        int jlen = build_json(json_buf, sizeof json_buf, ts, this_seq, pl);
        if (jlen < 0) {
            fprintf(stderr, "JSON overflow\n");
            return 1;
        }

        int pr = try_publish(zo_ring_a(g_zo), json_buf, (uint32_t)jlen);
        if (pr < 0)
            return 1;

        if (pr == 1) {
            dropped++;
            received += (uint64_t)drain_available(zo_ring_b(g_zo));
        } else {
            g_zo->bell.seq_in = this_seq;
            published++;

            if (g_verbose) {
                printf(ZO_CLR_RED
                       "[Host W] seq=%u ts=%llu len=%d\n"
                       "[Host W] %s\n"
                       ZO_CLR_RESET,
                       this_seq, (unsigned long long)ts, jlen - 1, json_buf);
                zo_print_sep();
            }

            received += (uint64_t)drain_available(zo_ring_b(g_zo));
        }

        if (g_verbose) {
            double now = zo_now_sec();
            if (now - t_last_report >= 1.0) {
                double elapsed = now - t0;
                double rate = elapsed > 0.0 ? (double)published / elapsed : 0.0;
                printf("[Host  ] rate=%.0f pkt/s  published=%llu  dropped=%llu  "
                       "received=%llu  queue_A=%u/%u\n",
                       rate,
                       (unsigned long long)published,
                       (unsigned long long)dropped,
                       (unsigned long long)received,
                       rb_count(zo_ring_a(g_zo)), rb_limit(zo_ring_a(g_zo)));
                t_last_report = now;
            }
        }
    }

    double t_done = zo_now_sec();
    double elapsed = t_done - t0;
    double rate = elapsed > 0.0 ? (double)published / elapsed : 0.0;

    if (g_verbose) {
        printf("publish window done: published=%llu dropped=%llu rate=%.0f pkt/s\n"
               "draining remaining results …\n",
               (unsigned long long)published,
               (unsigned long long)dropped, rate);
    }

    received += (uint64_t)drain_until_quiet(zo_ring_b(g_zo), 0.5, t_done + 5.0);

    printf("done: published=%llu dropped=%llu received=%llu "
           "rate=%.0f pkt/s slots=%u (%.2fs elapsed)\n",
           (unsigned long long)published,
           (unsigned long long)dropped,
           (unsigned long long)received,
           rate, slots, elapsed);

    if (published > 0 && received == 0) {
        fprintf(stderr,
                "no results received — is zynq_fpga_stub running?\n"
                "  terminal 1: ./zynq_fpga_stub -t %.0f\n"
                "  terminal 2: ./zynq_cpu_host  -t %.0f -s %u\n",
                seconds + 10.0, seconds, slots);
        return 2;
    }
    return 0;
}
