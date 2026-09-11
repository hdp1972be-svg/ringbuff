#ifndef RB_NOTIFY_H
#define RB_NOTIFY_H

#include "rb.h"

#if RB_ENABLE_NOTIFY

#ifdef __cplusplus
extern "C" {
#endif

/* Two independent wake-up channels:
 *   data  : producer -> consumer  ("new item published")
 *   space : consumer -> producer  ("slot released")
 *
 * Backends:
 *   RB_NOTIFY_EVENTFD  - Linux / Android (eventfd)
 *   RB_NOTIFY_PIPE     - any POSIX (pipe)
 *   RB_NOTIFY_NONE     - no-OS / polling (all ops are cheap no-ops)
 */
#ifndef RB_NOTIFY_BACKEND
#  if defined(__linux__) || defined(__ANDROID__)
#    define RB_NOTIFY_BACKEND RB_NOTIFY_EVENTFD
#  elif defined(_POSIX_VERSION)
#    define RB_NOTIFY_BACKEND RB_NOTIFY_PIPE
#  else
#    define RB_NOTIFY_BACKEND RB_NOTIFY_NONE
#  endif
#endif

#define RB_NOTIFY_EVENTFD 1
#define RB_NOTIFY_PIPE    2
#define RB_NOTIFY_NONE    3

typedef struct {
    int data_fd;    /* -1 if none */
    int space_fd;
} rb_notify_t;

rb_err_t rb_notify_init(rb_notify_t *n);
void     rb_notify_destroy(rb_notify_t *n);

/* Signals are cheap and idempotent; missed signals are OK as long as
   the waiter drains the ring fully after each wake-up. */
void rb_notify_signal_data (rb_notify_t *n);
void rb_notify_signal_space(rb_notify_t *n);

/* timeout_ms: <0 = block forever, 0 = poll once, >0 = bounded wait.
   Returns RB_OK on wakeup, RB_ERR_EMPTY on timeout. */
rb_err_t rb_notify_wait_data (rb_notify_t *n, int timeout_ms);
rb_err_t rb_notify_wait_space(rb_notify_t *n, int timeout_ms);

/* Raw fds for poll()/select()/epoll loops. -1 if unavailable. */
int rb_notify_data_fd (const rb_notify_t *n);
int rb_notify_space_fd(const rb_notify_t *n);

#ifdef __cplusplus
}
#endif
#endif /* RB_ENABLE_NOTIFY */
#endif /* RB_NOTIFY_H */
