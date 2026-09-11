#ifndef RB_THREAD_H
#define RB_THREAD_H

#include "rb.h"

#if RB_ENABLE_THREAD_HELPERS

#include <pthread.h>

typedef pthread_t rb_thread_t;
typedef void *(*rb_thread_fn)(void *arg);

/* Spawn a producer or consumer thread using the stack size set in the
   config or via rb_set_*_stack. is_producer selects which size applies.
   The thread function receives `arg` unchanged. */
rb_err_t rb_thread_spawn(rb_t *rb, rb_thread_t *out, bool is_producer,
                         rb_thread_fn fn, void *arg);

rb_err_t rb_thread_join(rb_thread_t *t);

#endif /* RB_ENABLE_THREAD_HELPERS */
#endif /* RB_THREAD_H */
