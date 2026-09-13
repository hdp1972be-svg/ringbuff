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

CMake's `PRIVATE` scope keeps `Threads::Threads` out of the `rb` target's usage requirements; the helper interface is the explicit opt-in path. citeturn0search0turn0search8

The installed package config may still locate `Threads` when thread helpers were built, because the exported optional helper target refers to it. That does **not** make `rb` itself link pthread.

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

The CMake build also exposes these as cache variables:

```bash
cmake -B build \
    -DRB_MEMCPY=my_fast_memcpy \
    -DRB_MEMSET=my_fast_memset
```

They are private build definitions for the library, so the override does not become a requirement of applications linking `rb`. CMake target compile definitions are target-specific unless marked `PUBLIC` or `INTERFACE`. citeturn1search0turn1search6

The default remains the standard `memcpy`/`memset` implementation. The hooks are intended mainly for embedded ports with a known platform-specific implementation.

For direct non-CMake compilation, define the hooks in the compiler invocation or port layer consistently with the selected implementation, for example:

```bash
cc -DRB_MEMCPY=my_fast_memcpy \
   -DRB_MEMSET=my_fast_memset \
   ...
```

---

## Notification backend: futex + eventfd

`RB_ENABLE_NOTIFY` enables the notification implementation directly in `src/rb.c`; there is no separate `rb_notify.c` implementation anymore.

The public declarations live in `include/rb.h`. `include/rb_notify.h` remains only as a compatibility header that includes `rb.h`.

The API is guarded by:

```c
#if RB_ENABLE_NOTIFY && defined(__linux__)
```

That means the backend is selected by the compiler's Linux platform definition rather than by a separate Android macro. **Android inherits this Linux path because Android defines `__linux__`**, while bare-metal targets do not and therefore do not expose the notification API.

The notification implementation uses:

- futexes for sleeping/waking a consumer thread;
- `eventfd` for fd-based integration with `poll`, `epoll`, libuv, libev, and similar event loops.

The feature is intended as a wake-up hint only. The ring's existing release/acquire publication remains the ownership and memory-ordering protocol.

Typical setup:

```c
int fd = rb_notify_fd(rb);
/* register fd with poll/epoll/event loop */
```

On readability:

```c
rb_notify_drain_fd(rb);
rb_drain(rb, consume_one, user);
```

For a sleeping consumer without an fd-based event loop:

```c
uint32_t expected = rb_notify_value(rb);
if (rb_count(rb) == 0)
    rb_wait(rb, expected, -1);
```

The consumer must always recheck/drain the ring after a wake-up. Notifications may be coalesced.

### Platform matrix

| Configuration | Core | Thread helpers | Notification |
|---|---|---|---|
| Linux | Yes | Optional pthread | Optional futex + eventfd |
| Android | Yes | Optional pthread/Bionic threads | Optional Linux futex + eventfd path |
| Other hosted POSIX | Yes | Optional, platform-dependent `Threads::Threads` | Not provided by this backend |
| Bare metal | Yes | Normally OFF | Not provided |

For embedded/bare-metal builds, use `RB_ENABLE_NOTIFY=OFF` and `RB_ENABLE_THREAD_HELPERS=OFF`.

---

## `rb_notify.h`

`include/rb_notify.h` is intentionally a compatibility shim:

```c
#include "rb.h"
```

New code should include `rb.h` directly. Keeping the header avoids unnecessarily breaking code that used the earlier separate notification header.

---

## Recommended embedded configuration

```bash
cmake -B build-embedded \
    -DRB_SINGLE_THREADED=ON \
    -DRB_ENABLE_THREAD_HELPERS=OFF \
    -DRB_ENABLE_NOTIFY=OFF \
    -DRB_ENABLE_STATS=OFF \
    -DRB_SIZE_OPTIMIZED=ON
```

If the platform supplies its own optimized memory primitives, additionally set `RB_MEMCPY` and `RB_MEMSET`.

---

## What the core deliberately does not depend on

The core `rb` target does not require:

- pthread;
- futexes;
- eventfd;
- an event loop;
- dynamic allocation;
- a particular Linux distribution;
- GPU/NIC/FPGA runtime libraries.

Device-visible memory, DMA synchronization, cache maintenance, fences, IOMMU mappings, and similar hardware-specific mechanisms remain the responsibility of the surrounding platform integration.

See [USE_CASES_NOTIFY.md](USE_CASES_NOTIFY.md) for the futex/eventfd usage patterns and [USE_CASES.md](USE_CASES.md) for the broader scratchpad use cases.
