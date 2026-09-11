How to use it in another program
1. Vendor it

Copy the whole tree, or add it as a git submodule, or add_subdirectory()
from your CMake project. There is no package manager step.
text

your-project/
├── third_party/
│   └── rb/             ← this repo
├── src/
└── CMakeLists.txt

2. Link from CMake
cmake

add_subdirectory(third_party/rb)

add_executable(myapp src/main.c)
target_link_libraries(myapp PRIVATE rb)

Or, if you want a different build configuration for the library:
cmake

set(RB_SLOT_SIZE  4096 CACHE STRING "" FORCE)
set(RB_NUM_SLOTS  256  CACHE STRING "" FORCE)
set(RB_CAPACITY   256  CACHE STRING "" FORCE)
set(RB_ENABLE_STATS ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/rb)

3. Allocate the control block and scratchpad

The library never allocates. You provide:

    the control block (size from rb_size(capacity)), aligned to
    RB_CACHE_LINE

    the scratchpad (slots * stride bytes), same alignment

c

#include "rb.h"
#include <stdlib.h>

#define CAPACITY   256u
#define SLOTS      256u
#define SLOT_SIZE  2048u

static void *aligned(size_t a, size_t n) {
    void *p = NULL;
    if (posix_memalign(&p, a, n)) abort();
    return p;
}

rb_t *rb = aligned(RB_CACHE_LINE, rb_size(CAPACITY));

size_t stride = SLOT_SIZE;                 /* already cache-line aligned */
size_t scratch_bytes = SLOTS * stride;
void  *scratch = aligned(RB_CACHE_LINE, scratch_bytes);

rb_config_t cfg;
rb_config_init(&cfg);
cfg.capacity  = CAPACITY;
cfg.slots     = SLOTS;
cfg.slot_size = SLOT_SIZE;

if (rb_init(rb, &cfg, scratch, scratch_bytes) != RB_OK) {
    /* handle */
}

On bare metal, replace aligned() with a static buffer:
c

static _Alignas(RB_CACHE_LINE) uint8_t rb_mem[rb_size(CAPACITY)];
static _Alignas(RB_CACHE_LINE) uint8_t scratch[SLOTS * SLOT_SIZE];

4. Wire it into a WS backpressure loop

This is the intended pattern: a socket reader produces, a transform step
consumes, and on_full / on_low_d toggle a stop_reading flag.
c

static volatile bool stop_reading = false;

static void on_full_cb(rb_t *rb, void *u)   { (void)rb; (void)u; stop_reading = true;  }
static void on_low_d_cb(rb_t *rb, void *u)  { (void)rb; (void)u; stop_reading = false; }

static void setup_callbacks(rb_t *rb) {
    rb_callbacks_t cb = {0};
    cb.on_full  = on_full_cb;
    cb.on_low_d = on_low_d_cb;
    rb_set_callbacks(rb, &cb);
    rb_set_low_d(rb, 25);   /* resume reading when ring drops below 25% */
}

/* Called when the socket becomes readable. */
static void on_socket_readable(int fd, rb_t *rb) {
    if (stop_reading) return;   /* backpressure: TCP window will do the rest */

    uint32_t idx, cap;
    void *w;
    rb_err_t e = rb_acquire(rb, MAX_FRAME, &idx, &w, &cap);
    if (e == RB_ERR_FULL) { stop_reading = true; return; }
    if (e != RB_OK)       return;

    ssize_t n = read(fd, w, cap);
    if (n > 0) rb_publish(rb, idx, (uint32_t)n);
    else       rb_abort(rb);
}

/* Called once per event-loop tick, or after each read batch. */
static bool transform(const void *obj, uint32_t len,
                      bool truncated, void *user)
{
    if (truncated) {
        /* decide: log, skip, or feed partial data downstream */
        return true;
    }
    process_frame(obj, len);
    return true;
}

static void drain_and_process(rb_t *rb) {
    rb_drain(rb, transform, NULL);
}

Two things to internalize:

    on_full and on_low_d are edge-triggered and latched. They
    fire once when the ring enters the region, not on every publish. The
    handler can safely do real work.

    The consumer never consults a callback. rb_drain runs to empty and
    returns.

5. Build and test
bash

# Native, with tests
cmake -B build -DRB_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# All variants (default, pointer, cachepad, TSan, ASan)
./run-all-checks.sh

# Cross-compile to ARM
cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/arm-linux-gnueabihf.cmake
cmake --build build-arm

# Bare-metal shape (no atomics, no threads, no notify)
cmake -B build-st \
  -DRB_SINGLE_THREADED=ON \
  -DRB_ENABLE_THREAD_HELPERS=OFF \
  -DRB_ENABLE_NOTIFY=OFF

6. Use it from a C++ project

The header is C++-safe (extern "C" guards are in place). Include rb.h
from your C++ code and link rb:
cpp

#include "rb.h"

extern "C" void rb_entry_point(rb_t *rb);

The library does not use C++ features, exceptions, or RTTI. It links into
a C++ binary without changes.
