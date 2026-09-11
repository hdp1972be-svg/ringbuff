# rb — Dependencies

[← Back to overview](../README.md) · [API reference](API.md) · [Benchmarks](BENCHMARKS.md) · [Technical notes](TECHNICAL.md)

Exactly what `rb` depends on, at every stage of the build and at
runtime. The short version is in the summary table; the rest of the
document explains each entry.

---

## Summary

| Stage | Dependency | Required? |
|---|---|---|
| Compile (core) | C11 compiler | Yes |
| Compile (core) | `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` | Yes |
| Compile (core) | `<stdatomic.h>` | Only if `RB_USE_ATOMICS=1` |
| Compile (core) | `<string.h>` | Yes (for `memcpy` / `memset`) |
| Compile (optional) | `<pthread.h>` | Only if `RB_ENABLE_THREAD_HELPERS=1` |
| Compile (optional) | `<sys/eventfd.h>` | Only if `RB_ENABLE_NOTIFY=1` on Linux |
| Link (core) | libc | Yes |
| Link (optional) | `libpthread` / `Threads::Threads` | Only if `RB_ENABLE_THREAD_HELPERS=1` |
| Build | CMake ≥ 3.16 | Yes |
| Runtime | None | — |

The core library has **no runtime dependencies beyond libc**. Everything
else is opt-in.

---

## Compile-time dependencies

### Standard C headers

`rb.c` includes exactly four headers:

```c
#include "rb.h"          /* public API */
#include "rb_port.h"     /* atomics, barriers, alignment */
#include <string.h>      /* memcpy, memset */
```

Transitively, `rb.h` pulls in:

- `<stddef.h>` — for `size_t`, `NULL`, `offsetof`
- `<stdint.h>` — for `uint32_t`, `uint64_t`, `uintptr_t`
- `<stdbool.h>` — for `bool`, `true`, `false`

All three are freestanding C11 headers. They are available in any
conforming C environment, including bare-metal toolchains that ship
only the freestanding subset (no `libc` at all).

### `<stdatomic.h>` (conditional)

`rb_port.h` includes `<stdatomic.h>` when `RB_USE_ATOMICS=1` (the
default). This is a C11 header. It is available on:

- GCC ≥ 4.9 with a target that supports atomics
- Clang ≥ 3.6 with the same
- Most modern ARM, x86, RISC-V, and MIPS toolchains

It is **not** available on some older embedded toolchains (notably
older ARMCC and IAR compilers). For those targets, set
`RB_USE_ATOMICS=0` and provide barriers via `rb_port.h`.

### `<pthread.h>` (optional)

`rb_thread.c` includes `<pthread.h>`. It is compiled only when
`RB_ENABLE_THREAD_HELPERS=1`. Setting the option to `OFF` removes the
include entirely — the file is not built, and `rb_thread.h` compiles to
an empty translation unit.

This is the single most important option for embedded targets.

### `<sys/eventfd.h>` (optional)

`rb_notify.c` includes `<sys/eventfd.h>` when
`RB_NOTIFY_BACKEND=RB_NOTIFY_EVENTFD` (the default on Linux and
Android). It is compiled only when `RB_ENABLE_NOTIFY=1`, which is
`OFF` by default.

`RB_NOTIFY_BACKEND=RB_NOTIFY_NONE` compiles the same file with all
operations reduced to no-ops, requiring no system headers at all.

---

## Runtime dependencies

### libc

The core library calls two libc functions:

- `memcpy` — for the 4-byte slot header write in `rb_publish_ex`, and
  the 4-byte header read in `rb_consume`
- `memset` — for zeroing the control block in `rb_init`, and the stats
  struct

Both are `<string.h>` functions. On freestanding environments (bare
metal), these are typically provided by the compiler's own builtins or
by a small `libc` replacement like newlib-nano, picolibc, or musl.

**The 4-byte `memcpy` calls are almost always inlined** by the
compiler into a single `mov` instruction. There is no measurable
runtime dependency on an optimized `memcpy` for the library's own use.
The payload copy — which is where `memcpy` performance actually matters
— is the caller's responsibility.

See [Overriding memcpy / memset](#overriding-memcpy--memset) below for
how to substitute your own implementations.

### No libpthread, no librt, no libm, no libdl

The core library does not link against any of these. It has no
dependency on POSIX, Linux, or any specific operating system.

### No runtime initialization

There is no constructor, no `__attribute__((constructor))`, no `atexit`
handler, no global initializer. The library is inert until `rb_init` is
called.

---

## Build-time dependencies

### CMake

CMake ≥ 3.16 is required. The version is chosen because
`target_link_libraries` with generator expressions, `CMAKE_C_STANDARD`
propagation, and `configure_package_config_file` are all stable from
that point.

### `Threads::Threads` (optional)

When `RB_ENABLE_THREAD_HELPERS=1`, the build calls
`find_package(Threads REQUIRED)`. This is a CMake module that locates
the platform's threading library (pthread on Linux and Android,
`-lpthread` or a libc-provided implementation elsewhere).

**Important:** the `Threads::Threads` target should not propagate to
consumers who don't use the thread helpers. See
[The pthread question](#the-pthread-question) below.

### C compiler

GCC or Clang are the primary supported compilers. The library uses:

- `_Alignas` / `_Alignof` (C11, or `__attribute__((aligned))` fallback)
- `_Atomic` (C11, gated by `RB_USE_ATOMICS`)
- Compound literals, designated initializers (C99, universally supported)

MSVC support is not tested. It may work with `/std:c11` on recent
versions, but the atomics implementation differs and has not been
validated.

---

## Dependency matrix by configuration

| Configuration | Headers | libc | pthread | OS |
|---|---|---|---|---|
| **Default** (`RB_USE_ATOMICS=1`, thread helpers ON) | `<stdatomic.h>`, `<string.h>`, `<pthread.h>` | Yes | Yes | Linux/Android |
| **No threads** (`RB_ENABLE_THREAD_HELPERS=OFF`) | `<stdatomic.h>`, `<string.h>` | Yes | No | Any POSIX |
| **Single-threaded** (`RB_SINGLE_THREADED=1`) | `<string.h>` | Yes | No | Any C11 |
| **Bare metal** (`RB_SINGLE_THREADED=1`, thread helpers OFF, atomics OFF, stats OFF) | `<string.h>` | Optional | No | Freestanding |
| **With notify** (`RB_ENABLE_NOTIFY=1`) | + `<sys/eventfd.h>` | Yes | No | Linux/Android |

The **bare metal** row is what you want for MCU targets. It reduces the
dependency surface to `<string.h>` and whatever provides it, and
nothing else.

---

## The pthread question

`rb_thread.c` exists to save the caller from writing the same
`pthread_create` boilerplate. It is a convenience, not a core feature.

**The problem:** in the current build, `Threads::Threads` is linked as
`PUBLIC`, which means every consumer — even one that only calls
`rb_acquire` and `rb_publish` — inherits the pthread dependency.

**The fix:** separate the helper into its own interface target so
consumers opt in explicitly.

```cmake
# Top-level CMakeLists.txt
if (RB_ENABLE_THREAD_HELPERS)
    target_sources(rb PRIVATE src/rb_thread.c)
    find_package(Threads REQUIRED)

    add_library(rb_thread_helpers INTERFACE)
    target_link_libraries(rb_thread_helpers INTERFACE Threads::Threads)
    target_link_libraries(rb_thread_helpers INTERFACE rb)
endif()
```

Consumers then choose:

```cmake
# Ring only — no pthread
target_link_libraries(myapp PRIVATE rb)

# Ring + thread helpers — pthread pulled in
target_link_libraries(myapp PRIVATE rb_thread_helpers)
```

This is the same pattern used by liblzma and several other C libraries
that have optional threading support.

### Verifying the propagation

After configuring, check the generated package config:

```bash
grep -n 'find_dependency' install/lib/cmake/rb/rbConfig.cmake
```

If it lists `Threads` unconditionally, the propagation is still there.
The `rbConfig.cmake.in` should gate it:

```cmake
set(_rb_needs_threads @RB_ENABLE_THREAD_HELPERS@)
if(_rb_needs_threads)
    find_dependency(Threads)
endif()
```

---

## The memcpy / memset question

The library calls `memcpy` and `memset` from `<string.h>`. On most
platforms these are the right choice: glibc, musl, and newlib all ship
optimized assembly versions, and the compiler inlines the small copies.

**However**, on some embedded toolchains, the default `memcpy` is a
naive byte-wise loop. This is a problem for *payload* copies the caller
does — not for the library's 4-byte header writes.

To give embedded consumers control, the library provides override hooks:

```c
/* rb_port.h */
#ifndef RB_MEMCPY
#  define RB_MEMCPY  memcpy
#endif
#ifndef RB_MEMSET
#  define RB_MEMSET  memset
#endif
```

`rb.c` uses `RB_MEMCPY` and `RB_MEMSET` everywhere. An embedded
consumer with a slow newlib can supply their own:

```bash
cmake ... -DRB_MEMCPY=my_fast_memcpy -DRB_MEMSET=my_fast_memset
```

or via direct defines:

```bash
cc -DRB_MEMCPY=arm_fast_memcpy -DRB_MEMSET=arm_fast_memset ...
```

### What we deliberately do not ship

Hand-written assembly `memcpy`/`memset` implementations. Reasons:

1. On x86-64, glibc already uses AVX2/ERMS with runtime CPU dispatch.
   A hand-written version would be slower.
2. On ARM, the GNU toolchain's newlib and the NDK both ship optimized
   assembly versions. Writing our own would duplicate well-tested code.
3. For the library's own 4-byte copies, the compiler already inlines
   them to a single instruction. No `memcpy` call happens at all.
4. Shipping a hook costs nothing and lets the consumer decide. Shipping
   assembly costs maintenance and gets it wrong on some microarchitecture.

---

## Embedded targets

For an MCU target (Cortex-M, RISC-V, etc.), the recommended build flags are:

```bash
cmake -B build-embedded \
    -DRB_SINGLE_THREADED=ON \
    -DRB_ENABLE_THREAD_HELPERS=OFF \
    -DRB_ENABLE_NOTIFY=OFF \
    -DRB_ENABLE_STATS=OFF \
    -DRB_SIZE_OPTIMIZED=ON \
    -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections"
```

This configuration:

- removes atomics and barriers (`RB_SINGLE_THREADED=1`)
- removes `<pthread.h>` (`RB_ENABLE_THREAD_HELPERS=OFF`)
- removes `<sys/eventfd.h>` (`RB_ENABLE_NOTIFY=OFF`)
- removes stats fields from the control block (`RB_ENABLE_STATS=OFF`)
- compiles with `-Os` (`RB_SIZE_OPTIMIZED=ON`)
- enables per-function sections so unused functions can be discarded
- discards them at link time (`--gc-sections`)

The resulting linked footprint is typically under 2 KB of `.text` and
0 bytes of `.data` and `.bss`.

The only remaining dependency is `<string.h>`. If your toolchain does
not provide it, define `RB_MEMCPY` and `RB_MEMSET` to your own
implementations and the include can be removed by a small patch.

---

## Verifying the dependency graph

To see what the library actually pulls in:

```bash
# Object file level
nm -u build-default/librb.a

# Linked binary level
ldd ./your_program

# CMake-level dependency tree
cmake --graphviz=deps.dot build-default
dot -Tpng deps.dot -o deps.png
```

`nm -u` lists undefined symbols — what the library expects to find at
link time. For the core library this should show `memcpy`, `memset`,
and the atomic builtins (`__atomic_load`, `__atomic_store`, etc. on
GCC before 11, or nothing on GCC 11+ where they are inlined).

If `nm -u` shows `pthread_create` or `eventfd`, you have a configuration
that enables the corresponding optional module.

---

## What we deliberately do not depend on

- **Any specific libc.** The library uses only the standard C11 headers
  and two `<string.h>` functions. It works with glibc, musl, newlib,
  picolibc, uClibc, Bionic, and any conforming replacement.
- **Any OS.** No POSIX, no Linux, no Windows-specific code in the core.
- **Any build system other than CMake.** Meson, Bazel, and hand-written
  Makefiles are not supported. If you need one, the two source files
  (`rb.c`, and optionally `rb_thread.c` / `rb_notify.c`) can be compiled
  directly.
- **Any runtime library beyond libc.** No `librt`, no `libm`, no
  `libdl`, no `libgcc_s` (unless your compiler emits calls to it for
  64-bit division; use `-msoft-div` or equivalent on targets without
   hardware divide).
- **Any data segment.** The library has zero global state. Everything
  lives in caller-provided memory.

---

## See also

- [API reference](API.md) — how to use the library.
- [Technical notes](TECHNICAL.md) — design rationale and porting.
- [Benchmarks](BENCHMARKS.md) — performance in the default configuration.
- [Overview](../README.md) — what it is and why.
