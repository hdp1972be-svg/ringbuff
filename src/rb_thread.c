/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */

#include "rb_thread.h"
#include "rb_port.h"   /* for RB_ALIGN_UP */

#if RB_ENABLE_THREAD_HELPERS

#include <pthread.h>
#include <unistd.h>

static size_t page_align_up(size_t n) {
    long p = sysconf(_SC_PAGESIZE);
    size_t ps = (p > 0) ? (size_t)p : 4096u;
    return RB_ALIGN_UP(n, ps);
}

rb_err_t rb_thread_spawn(rb_t *rb, rb_thread_t *out, bool is_producer,
                         rb_thread_fn fn, void *arg)
{
    if (!rb || !out || !fn) return RB_ERR_INVAL;

    size_t want = is_producer ? rb_producer_stack(rb)
                              : rb_consumer_stack(rb);
    if (want < RB_THREAD_MIN_STACK) want = RB_THREAD_MIN_STACK;
    want = page_align_up(want);

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return RB_ERR_INVAL;
    if (pthread_attr_setstacksize(&attr, want) != 0) {
        pthread_attr_destroy(&attr);
        return RB_ERR_INVAL;
    }
    int rc = pthread_create(out, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0 ? RB_OK : RB_ERR_INVAL;
}

rb_err_t rb_thread_join(rb_thread_t *t) {
    if (!t) return RB_ERR_INVAL;
    return pthread_join(*t, NULL) == 0 ? RB_OK : RB_ERR_INVAL;
}

#endif /* RB_ENABLE_THREAD_HELPERS */
