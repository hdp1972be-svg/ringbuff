#define _POSIX_C_SOURCE 200809L

#include "rb.h"
#include "rb_thread.h"

#include <uv.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CAPACITY  64u
#define SLOTS     64u
#define SLOT_SIZE 1024u

typedef enum {
    MODE_POLL = 0,
    MODE_FUTEX,
    MODE_UV,
} bench_mode_t;

typedef struct {
    rb_t *rb;
    int devnull;
    uint32_t msg_size;
    bench_mode_t mode;
    atomic_uint_fast64_t produced;
    atomic_uint_fast64_t consumed;
    atomic_uint_fast64_t bytes;
    atomic_bool stop;
} ctx_t;

typedef struct {
    ctx_t *ctx;
    uv_loop_t loop;
    uv_poll_t poller;
    uv_timer_t timer;
} uv_state_t;

static void *aligned_or_die(size_t align, size_t sz) {
    void *p = NULL;
    if (posix_memalign(&p, align, sz) != 0 || !p) {
        fprintf(stderr, "oom\n");
        exit(2);
    }
    return p;
}

static bool on_item(const void *obj, uint32_t len, bool trunc, void *user) {
    ctx_t *c = user;
    if (trunc) return true;
    ssize_t w = write(c->devnull, obj, len);
    (void)w;
    atomic_fetch_add(&c->consumed, 1);
    atomic_fetch_add(&c->bytes, len);
    return true;
}

static void consume_all(ctx_t *c) {
    rb_drain(c->rb, on_item, c);
}

static void *producer_fn(void *arg) {
    ctx_t *c = arg;
    void *buf = malloc(c->msg_size);
    int rfd = open("/dev/random", O_RDONLY);
    if (!buf || rfd < 0) {
        fprintf(stderr, "producer setup failed\n");
        _exit(1);
    }
    while (!atomic_load(&c->stop)) {
        uint32_t idx, cap;
        void *w;
        if (rb_acquire(c->rb, c->msg_size, &idx, &w, &cap) == RB_ERR_FULL) {
            sched_yield();
            continue;
        }
        ssize_t n = read(rfd, buf, c->msg_size);
        if (n <= 0) {
            rb_abort(c->rb);
            break;
        }
        memcpy(w, buf, (size_t)n);
        rb_publish(c->rb, idx, (uint32_t)n);
        atomic_fetch_add(&c->produced, 1);
    }
    close(rfd);
    free(buf);
    return NULL;
}

static void *consumer_poll_fn(void *arg) {
    ctx_t *c = arg;
    while (!atomic_load(&c->stop)) {
        consume_all(c);
    }
    consume_all(c);
    return NULL;
}

static void *consumer_futex_fn(void *arg) {
    ctx_t *c = arg;
    for (;;) {
        uint32_t v = rb_notify_value(c->rb);
        consume_all(c);
        if (atomic_load(&c->stop)) break;
        rb_wait(c->rb, v, 100);
    }
    return NULL;
}

static void uv_on_ready(uv_poll_t *p, int status, int events) {
    (void)status;
    (void)events;
    uv_state_t *s = p->data;
    for (;;) {
        rb_notify_drain_fd(s->ctx->rb);
        consume_all(s->ctx);
        if (rb_count(s->ctx->rb) == 0u) break;
    }
}

static void uv_on_tick(uv_timer_t *t) {
    uv_state_t *s = t->data;
    if (atomic_load(&s->ctx->stop)) {
        uv_stop(&s->loop);
    }
}

static void *consumer_uv_fn(void *arg) {
    ctx_t *c = arg;
    uv_state_t st;
    memset(&st, 0, sizeof st);
    st.ctx = c;
    if (uv_loop_init(&st.loop) != 0) return NULL;
    int fd = rb_notify_fd(c->rb);
    if (fd < 0) return NULL;
    if (uv_poll_init(&st.loop, &st.poller, fd) != 0) return NULL;
    st.poller.data = &st;
    if (uv_poll_start(&st.poller, UV_READABLE, uv_on_ready) != 0) return NULL;
    uv_timer_init(&st.loop, &st.timer);
    st.timer.data = &st;
    if (uv_timer_start(&st.timer, uv_on_tick, 100, 100) != 0) return NULL;
    uv_run(&st.loop, UV_RUN_DEFAULT);
    uv_close((uv_handle_t *)&st.poller, NULL);
    uv_close((uv_handle_t *)&st.timer, NULL);
    uv_run(&st.loop, UV_RUN_NOWAIT);
    uv_loop_close(&st.loop);
    return NULL;
}

static bench_mode_t parse_mode(const char *s) {
    if (!strcmp(s, "poll"))  return MODE_POLL;
    if (!strcmp(s, "futex")) return MODE_FUTEX;
    if (!strcmp(s, "uv"))    return MODE_UV;
    return MODE_POLL;
}

static const char *mode_name(bench_mode_t m) {
    switch (m) {
    case MODE_FUTEX: return "futex";
    case MODE_UV:    return "uv";
    default:         return "poll";
    }
}

static void usage(const char *p) {
    fprintf(stderr, "usage: %s -m <poll|futex|uv> [-t seconds] [-s bytes]\n", p);
}

int main(int argc, char **argv) {
    bench_mode_t mode = MODE_POLL;
    int seconds = 5;
    uint32_t msg_size = 512u;
    int opt;

    while ((opt = getopt(argc, argv, "m:t:s:h")) != -1) {
        switch (opt) {
        case 'm':
            mode = parse_mode(optarg);
            break;
        case 't':
            seconds = atoi(optarg);
            break;
        case 's':
            msg_size = (uint32_t)atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (seconds <= 0 || msg_size == 0 || msg_size > SLOT_SIZE) {
        usage(argv[0]);
        return 1;
    }

    void *rb_mem  = aligned_or_die(RB_CACHE_LINE, rb_size(CAPACITY));
    void *scratch = aligned_or_die(RB_CACHE_LINE, (size_t)SLOTS * SLOT_SIZE);
    rb_t *rb = (rb_t *)rb_mem;

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = CAPACITY;
    cfg.slots     = SLOTS;
    cfg.slot_size = SLOT_SIZE;

    if (rb_init(rb, &cfg, scratch, SLOTS * SLOT_SIZE) != RB_OK) {
        fprintf(stderr, "rb_init failed\n");
        return 1;
    }

    rb_notify_fd(rb);

    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rb = rb;
    ctx.msg_size = msg_size;
    ctx.mode = mode;
    ctx.devnull = open("/dev/null", O_WRONLY);
    if (ctx.devnull < 0) {
        fprintf(stderr, "open /dev/null failed\n");
        return 1;
    }
    atomic_init(&ctx.produced, 0);
    atomic_init(&ctx.consumed, 0);
    atomic_init(&ctx.bytes, 0);
    atomic_init(&ctx.stop, false);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    rb_thread_t tp, tc;
    if (rb_thread_spawn(rb, &tp, true, producer_fn, &ctx) != RB_OK) {
        fprintf(stderr, "producer spawn failed\n");
        return 1;
    }
    switch (mode) {
    case MODE_FUTEX: rb_thread_spawn(rb, &tc, false, consumer_futex_fn, &ctx); break;
    case MODE_UV:    rb_thread_spawn(rb, &tc, false, consumer_uv_fn, &ctx);    break;
    default:         rb_thread_spawn(rb, &tc, false, consumer_poll_fn, &ctx);  break;
    }

    struct timespec ts = { seconds, 0 };
    nanosleep(&ts, NULL);
    atomic_store(&ctx.stop, true);

    rb_thread_join(&tp);
    rb_thread_join(&tc);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    unsigned long long produced = (unsigned long long)atomic_load(&ctx.produced);
    unsigned long long consumed = (unsigned long long)atomic_load(&ctx.consumed);
    unsigned long long bytes    = (unsigned long long)atomic_load(&ctx.bytes);

    printf("%-6s produced=%llu consumed=%llu bytes=%llu in %.2fs\n",
           mode_name(mode), produced, consumed, bytes, secs);
    printf("        msg/s=%12.1f   MB/s=%9.2f\n",
           (double)consumed / secs,
           (double)bytes / secs / 1e6);

    close(ctx.devnull);
    rb_deinit(rb);
    free(scratch);
    free(rb_mem);
    return 0;
}