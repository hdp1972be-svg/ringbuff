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

static void sleep_us(long us) {
    struct timespec ts = { .tv_sec = us / 1000000,
                           .tv_nsec = (us % 1000000) * 1000L };
    nanosleep(&ts, NULL);
}

int main(void) {
    signal(SIGINT, on_sigint);

    /* Create the region. O_EXCL protects against a stale shm segment
       from a previous run; the user must `rm /dev/shm/rb_ipc_demo`. */
    int fd = shm_open(RB_SHM_NAME, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            fprintf(stderr,
                    "writer: %s already exists. remove it with:\n"
                    "        rm /dev/shm%s\n",
                    RB_SHM_NAME, RB_SHM_NAME);
        } else {
            perror("writer: shm_open");
        }
        return 1;
    }

    size_t sz = ipc_region_size();
    if (ftruncate(fd, (off_t)sz) != 0) {
        perror("writer: ftruncate");
        close(fd);
        shm_unlink(RB_SHM_NAME);
        return 1;
    }

    void *base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("writer: mmap");
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
        fprintf(stderr, "writer: rb_init failed: %d\n", (int)e);
        munmap(base, sz);
        shm_unlink(RB_SHM_NAME);
        return 1;
    }

    printf("writer: pid=%d shm=%s size=%zu B\n",
           (int)getpid(), RB_SHM_NAME, sz);
    printf("writer: publishing %u messages, Ctrl-C to stop early\n",
           RB_TOTAL_MESSAGES);

    uint64_t seq = 0;
    uint64_t full_waits = 0;

    while (!g_stop && seq < RB_TOTAL_MESSAGES) {
        char msg[192];
        int len = snprintf(msg, sizeof msg,
                           "msg #%llu from pid %d at %lld",
                           (unsigned long long)seq,
                           (int)getpid(),
                           (long long)time(NULL));
        if (len <= 0) break;

        uint32_t idx, cap;
        void *w;
        e = rb_acquire(rb, (uint32_t)len, &idx, &w, &cap);
        if (e == RB_ERR_FULL) {
            /* Reader is behind. Back off, retry. This is the ring's
               backpressure signal in action. */
            full_waits++;
            sleep_us(200);
            continue;
        }
        if (e != RB_OK) {
            fprintf(stderr, "writer: acquire error %d\n", (int)e);
            break;
        }

        memcpy(w, msg, (size_t)len);
        rb_publish(rb, idx, (uint32_t)len);
        seq++;

        if ((seq % 10000u) == 0u)
            printf("writer: %llu published (full_waits=%llu)\n",
                   (unsigned long long)seq,
                   (unsigned long long)full_waits);

        /* Small delay so the demo prints interesting numbers.
           Remove for a pure throughput test. */
        sleep_us(20);
    }

    printf("writer: done. %llu published, %llu backpressure waits\n",
           (unsigned long long)seq,
           (unsigned long long)full_waits);

    rb_deinit(rb);
    munmap(base, sz);
    /* Deliberately NOT calling shm_unlink here: the reader may still
       be attached. Cleanup is documented in examples/README.md. */
    return 0;
}
