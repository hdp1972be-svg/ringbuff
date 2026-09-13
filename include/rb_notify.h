/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 H. De Pauw */
#ifndef RB_NOTIFY_H
#define RB_NOTIFY_H

/* Notification support now lives directly in rb.c and rb.h.
 * Enable it with -DRB_ENABLE_NOTIFY=1 (or CMake -DRB_ENABLE_NOTIFY=ON).
 * On Linux this provides rb_wait()/rb_notify_value() for futex waiting and
 * rb_notify_fd()/rb_notify_drain_fd() for event-loop integration. */
#include "rb.h"

#endif /* RB_NOTIFY_H */
