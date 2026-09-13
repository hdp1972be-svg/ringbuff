/* SPDX-License-Identifier: MIT */
#include "rb.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

static rb_t *g_rb;
static volatile int g_done;

static void *consumer(void *arg) {
    (void)arg;
    uint32_t expected = rb_notify_value(g_rb);
    while (!g_done) {
        if (rb_count(g_rb) != 0u) break;
        int rc = rb_wait(g_rb, expected, 3000);
        if (rc == -ETIMEDOUT) return (void *)(intptr_t)2;
        if (rc < 0 && rc != -EINTR) return (void *)(intptr_t)3;
        expected = rb_notify_value(g_rb);
    }
    uint32_t slot = 0, len = 0;
    const void *obj = NULL;
    bool truncated = false;
    if (rb_consume(g_rb, &slot, &obj, &len, &truncated) != RB_OK) return (void *)(intptr_t)4;
    if (len != 5u || truncated || memcmp(obj, "hello", 5) != 0) return (void *)(intptr_t)5;
    if (rb_release(g_rb, slot) != RB_OK) return (void *)(intptr_t)6;
    return 0;
}

int main(void) {
    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity = 8;
    cfg.slots = 8;
    cfg.slot_size = 64;

    rb_t *rb = NULL;
    size_t control_size = rb_size(cfg.capacity);
    if (posix_memalign((void **)&rb, 64, control_size) != 0) return 10;
    void *scratch = NULL;
    if (posix_memalign(&scratch, 64, (size_t)cfg.slots * 64u) != 0) return 11;
    if (rb_init(rb, &cfg, scratch, (size_t)cfg.slots * 64u) != RB_OK) return 12;
    g_rb = rb;

    pthread_t th;
    if (pthread_create(&th, NULL, consumer, NULL) != 0) return 13;
    usleep(20000);

    uint32_t slot = 0, cap = 0;
    void *w = NULL;
    if (rb_acquire(rb, 5, &slot, &w, &cap) != RB_OK) return 14;
    memcpy(w, "hello", 5);
    if (rb_publish(rb, slot, 5) != RB_OK) return 15;

    void *thread_rc = NULL;
    pthread_join(th, &thread_rc);
    if ((intptr_t)thread_rc != 0) return 16;

    int fd = rb_notify_fd(rb);
    if (fd < 0) return 17;
    if (rb_acquire(rb, 1, &slot, &w, &cap) != RB_OK) return 18;
    *(unsigned char *)w = 0x42;
    if (rb_publish(rb, slot, 1) != RB_OK) return 19;

    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 1000) != 1 || !(pfd.revents & POLLIN)) return 20;
    if (rb_notify_drain_fd(rb) != 0) return 21;
    if (rb_count(rb) != 1u) return 22;

    if (rb_consume(rb, &slot, &w, &cap, NULL) != RB_OK) return 23;
    if (rb_release(rb, slot) != RB_OK) return 24;

    g_done = 1;
    rb_deinit(rb);
    free(scratch);
    free(rb);
    puts("PASS: futex sleep/wake and eventfd poll notification");
    return 0;
}
