#define _GNU_SOURCE
#include "rb.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static rb_t *g_rb;
static volatile sig_atomic_t g_signal_seen;
static volatile int g_waiting;

static void on_signal(int sig) {
    (void)sig;
    g_signal_seen = 1;
}

static void *waiter(void *arg) {
    (void)arg;
    uint32_t expected = rb_notify_value(g_rb);
    g_waiting = 1;
    int rc = rb_wait(g_rb, expected, 1000);
    g_waiting = 0;
    return (void *)(intptr_t)rc;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0) return 10;

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity = 8;
    cfg.slots = 8;
    cfg.slot_size = 64;

    rb_t *rb = NULL;
    void *scratch = NULL;
    if (posix_memalign((void **)&rb, 64, rb_size(cfg.capacity)) != 0) return 11;
    if (posix_memalign(&scratch, 64, (size_t)cfg.slots * cfg.slot_size) != 0) return 12;
    if (rb_init(rb, &cfg, scratch, (size_t)cfg.slots * cfg.slot_size) != RB_OK) return 13;
    g_rb = rb;

    pthread_t th;
    if (pthread_create(&th, NULL, waiter, NULL) != 0) return 14;
    while (!g_waiting) sched_yield();
    usleep(20000);

    if (pthread_kill(th, SIGUSR1) != 0) return 15;
    usleep(20000);

    /* The signal must interrupt the kernel futex wait, but rb_wait() must
       transparently retry it rather than leaking EINTR to its caller. */
    if (!g_signal_seen || !g_waiting) return 16;

    uint32_t slot, cap;
    void *w;
    if (rb_acquire(rb, 5, &slot, &w, &cap) != RB_OK) return 17;
    memcpy(w, "hello", 5);
    if (rb_publish(rb, slot, 5) != RB_OK) return 18;

    void *thread_rc = NULL;
    if (pthread_join(th, &thread_rc) != 0) return 19;
    if ((intptr_t)thread_rc != 0) return 20;

    const void *obj;
    uint32_t len;
    bool truncated;
    if (rb_consume(rb, &slot, &obj, &len, &truncated) != RB_OK) return 21;
    if (len != 5 || truncated || memcmp(obj, "hello", 5) != 0) return 22;
    if (rb_release(rb, slot) != RB_OK) return 23;

    rb_deinit(rb);
    free(scratch);
    free(rb);
    puts("PASS: SIGUSR1 interrupts futex wait; rb_wait retries EINTR and wakes normally");
    return 0;
}
