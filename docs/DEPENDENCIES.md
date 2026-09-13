# rb — Dependencies

[← Back to overview](../README.md) · [API reference](API.md) · [Benchmarks](BENCHMARKS.md) · [Technical notes](TECHNICAL.md)

This document describes the dependencies of the current implementation, including the optional pthread helpers, Linux/Android notification backend, and configurable memory hooks.

## Summary

| Stage | Dependency | Required? |
|---|---|---|
| Compile (core) | C11 compiler | Yes |
| Compile (core) | `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` | Yes |
| Compile (core) | `<string.h>` | Yes, unless the port layer supplies equivalent declarations |
| Compile (core) | `<stdatomic.h>` | Only with `RB_USE_ATOMICS=1` |
| Compile (thread helpers) | `<pthread.h>` | Only with `RB_ENABLE_THREAD_HELPERS=1` |
| Compile (notify) | Linux futex/eventfd headers | Only with `RB_ENABLE_NOTIFY=1` on Linux/Android |
| Link (core) | libc / C runtime | Normal hosted C build |
| Link (thread helpers) | `Threads::Threads` | Only when `rb_thread_helpers` is linked |
| Build | CMake ≥ 3.16 | Yes for the CMake build |

The important property is that **the core `rb` target does not link pthread**. Thread support is a separate opt-in target.

---

## NUMA and CPU affinity

The SPSC protocol is cache-friendly when producer and consumer execute on the same NUMA node. On multi-socket systems, pin the producer and consumer to cores on the same socket when latency matters, and allocate the scratchpad from the consumer's NUMA node when the consumer is the dominant reader of payload data.

Cross-node cache-line transfers can materially increase latency; the exact penalty is hardware- and workload-dependent, so benchmark the target machine rather than relying on a fixed nanosecond figure.

These are placement recommendations, not library requirements. The ring remains correct without affinity or NUMA configuration.

---

## Core library

`rb` is built from `src/rb.c` and has no dependency on `pthread`, `eventfd`, or an event loop.

The normal core path uses C11 atomics when `RB_USE_ATOMICS=1`. `RB_SINGLE_THREADED=1` removes the atomic implementation from the normal single-threaded build.

The core source uses `<string.h>` for its small control/header copies and zeroing operations. These operations can be redirected through `RB_MEMCPY` and `RB_MEMSET`; see below.

---

## Thread helpers and pthread propagation

`rb_thread.c` is an optional convenience layer for spawning/joining the library's helper threads.

The CMake build deliberately keeps pthread out of the core target:

```cmake
add_library(rb STATIC src/rb.c)

if (RB_ENABLE_THREAD_HELPERS)
    find_package(Threads REQUIRED)

    add_library(rb_thread STATIC src/rb_thread.c)
    target_link_libraries(rb_thread PRIVATE Threads::Threads)
    target_link_libraries(rb_thread PRIVATE rb)

    add_library(rb_thread_helpers INTERFACE)
    target_link_libraries(rb_thread_helpers INTERFACE rb_thread)
endif()
```

Consumers therefore choose explicitly:

```cmake
# Ring only — no pthread requirement from rb.
target_link_libraries(myapp PRIVATE rb)

# Ring + thread helpers — opt into the pthread dependency.
target_link_libraries(myapp PRIVATE rb_thread_helpers)
```

---

## Memory-copy hooks

The portability layer provides:

```c
#ifndef RB_MEMCPY
#  define RB_MEMCPY memcpy
#endif
#ifndef RB_MEMSET
#  define RB_MEMSET memset
#endif
```

The CMake build also exposes these as cache variables. They are private build definitions for the library, so the override does not become a requirement of applications linking `rb`.

---

## Notification backend: futex + eventfd

`RB_ENABLE_NOTIFY` enables the notification implementation directly in `src/rb.c`. The public declarations live in `include/rb.h`; `include/rb_notify.h` remains a compatibility shim.

The API is guarded by:

```c
#if RB_ENABLE_NOTIFY && defined(__linux__)
```

The notification implementation uses futexes for sleeping/waking a consumer thread and `eventfd` for fd-based integration with `poll`, `epoll`, libuv, libev, and similar event loops.

The feature is a wake-up hint only. The ring's release/acquire publication remains the ownership and memory-ordering protocol. Consumers must always recheck/drain the ring after a wake-up.

---

## Platform matrix

| Configuration | Core | Thread helpers | Notification |
|---|---|---|---|
| Linux | Yes | Optional pthread | Optional futex + eventfd |
| Android | Yes | Optional pthread/Bionic threads | Optional Linux futex + eventfd path |
| Other hosted POSIX | Yes | Optional | Not provided by this backend |
| Bare metal | Yes | Normally OFF | Not provided |

For embedded/bare-metal builds, use `RB_ENABLE_NOTIFY=OFF` and `RB_ENABLE_THREAD_HELPERS=OFF`.
