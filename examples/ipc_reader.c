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

int main(void) {
    signal(SIGINT, on_sigint);

    int fd = shm_open(RB_SHM_NAME, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr,
                "reader: cannot open %s (start the writer first)\n",
                RB_SHM_NAME);
        perror("reader: shm_open");
        return 1;
    }

    size_t sz = ipc_region_size();
    void *base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("reader: mmap");
        close(fd);
        return 1;
    }
    close(fd);

    rb_t *rb = ipc_ring(base);

    printf("reader: pid=%d attached to %s\n", (int)getpid(), RB_SHM_NAME);
    printf("reader: waiting for data, Ctrl-C to stop\n");

    uint64_t received = 0;
    uint64_t last_seq = 0;
    uint64_t gaps = 0;
    uint64_t truncations = 0;
    uint64_t waits = 0;

    while (!g_stop) {
        uint32_t idx, len;
        const void *obj;
        bool trunc;

        rb_err_t e = rb_consume(rb, &idx, &obj, &len, &trunc);
        if (e == RB_ERR_EMPTY) {
            /* The ring is in MAP_SHARED memory. With RB_FUTEX_SHARED=1,
               rb_wait() uses a process-shared futex on the shared head. */
            uint32_t expected = rb_notify_value(rb);
            int w = rb_wait(rb, expected, 1000);
            waits++;
            if (w != 0 && w != -ETIMEDOUT && w != -EINTR) {
                fprintf(stderr, "reader: rb_wait error %d\n", w);
                break;
            }
            continue;
        }
        if (e != RB_OK) {
            fprintf(stderr, "reader: consume error %d\n", (int)e);
            break;
        }

        if (trunc) {
            truncations++;
        } else if (len > 0) {
            /* The payload starts with "msg #N ". Parse N and check it
               matches the expected sequence. This verifies no items
               were lost, duplicated, or reordered across processes. */
            uint64_t n = 0;
            if (sscanf((const char *)obj, "msg #%llu", (unsigned long long *)&n) == 1) {
                if (last_seq != 0 && n != last_seq + 1) {
                    gaps++;
                    if (gaps <= 5) {
                        fprintf(stderr,
                                "reader: sequence gap, expected %llu got %llu\n",
                                (unsigned long long)(last_seq + 1),
                                (unsigned long long)n);
                    }
                }
                last_seq = n;
            }
        }

        rb_release(rb, idx);
        received++;

        if ((received % 10000u) == 0u) {
            printf("reader: %llu received, last_seq=%llu gaps=%llu waits=%llu\n",
                   (unsigned long long)received,
                   (unsigned long long)last_seq,
                   (unsigned long long)gaps,
                   (unsigned long long)waits);
        }
    }

    printf("reader: stopping. received=%llu gaps=%llu truncations=%llu waits=%llu\n",
           (unsigned long long)received,
           (unsigned long long)gaps,
           (unsigned long long)truncations,
           (unsigned long long)waits);

    munmap(base, sz);
    return 0;
}
