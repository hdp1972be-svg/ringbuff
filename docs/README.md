# rb — SPSC ring buffer with scratchpad

A small, dependency-free C11 library that implements a **single-producer /
single-consumer** FIFO of *references* into a preallocated **scratchpad**.
The queue never holds the objects themselves — only indices (or pointers)
into fixed-size slots. Producer writes directly into a slot, publishes its
index; consumer reads directly from the same slot, then releases it.

Zero-copy both directions. No `memcpy` of the payload. Designed for
event-loop backpressure between a socket reader and a downstream transformer.

---

## Why this exists

A common shape: a fast producer (socket read path) feeds a slower consumer
(a transform, parse, or forwarding step). You need a bounded buffer that

- applies **backpressure** when full, instead of dropping or growing,
- is **cheap enough** to call on every frame without being the bottleneck,
- works **identically** on Linux, Android, and (later) bare metal,
- does **not** copy payload bytes more than once.

A queue of pointers into a preallocated arena is the natural fit. This
library provides exactly that, with an API small enough to fit in one head.

---

## The model

### Ring

A circular array of `capacity` entries. Each entry is:

- a `uint32_t` slot index (default), or
- a `void *` slot pointer (compile-time switch `RB_USE_POINTERS=1`).

`capacity` must be a power of two so the modulo is a mask.

### Scratchpad

A caller-owned block of `slots` slots, each `slot_size` bytes, stride-aligned.
Each slot begins with a 4-byte header:

```
 bit 31    : TRUNCATED flag
 bits 0-30 : payload length actually written
```

The payload begins at `slot + 4`. Effective payload capacity is
`slot_size - 4`.

### Zero-copy flow

```
producer                          consumer
--------                          --------
rb_acquire(&idx, &w, &cap)  ->    (slot becomes busy in producer's hands)
write into w[0..len)              |
rb_publish(idx, len)              |
                                  rb_consume(&idx, &obj, &len, &trunc)
                                  read from obj[0..len)
                                  rb_release(idx)
```

No data ever crosses the ring. Only indices.

### Layout

```
   ring control block                        scratchpad
  +--------------------+            +--------------------------+
  |  head (atomic)     |            |  slot 0: [hdr][payload]  |
  |  cached_tail       |            |  slot 1: [hdr][payload]  |
  |  pending_*         |            |  slot 2: [hdr][payload]  |
  |  full_latch        |            |  ...                     |
  |--------------------|            |  slot z-1: [hdr][payload]|
  |  tail (atomic)     |            +--------------------------+
  |  cached_head       |                     ^
  |  consumer_*        |                     |
  |  low_*_latch       |            entries[] hold indices into this
  |--------------------|
  |  capacity, limit   |
  |  slots, slot_size  |
  |  callbacks         |
  |  stats             |
  |--------------------|
  |  entries[capacity] |  --->  [ i0 | i1 | i2 | ... ]
  +--------------------+
```

`head` and `tail` are on separate cache lines. Producer only writes `head`;
consumer only writes `tail`. Each side caches the other's counter to avoid
unnecessary atomic loads.

---

## Callback semantics

All callbacks are **informational**. They never gate control flow, never
block, never re-enter the queue.

| Callback         | Fires in    | Trigger                              | Latched? |
|------------------|-------------|--------------------------------------|----------|
| `on_slot_added`  | producer    | every successful `rb_publish`        | no       |
| `on_full`        | producer    | count reaches `limit`                | yes      |
| `on_low_d`       | consumer    | count crosses below `d%` of `limit`  | yes      |
| `on_low_e`       | consumer    | count crosses below `e%` of `limit`  | yes      |

**Latched** means: fire once on entering the region, do not fire again until
the region is exited. `on_full` latches at `limit`, unlatches when the
consumer brings the count below the `low_d` threshold. `on_low_d` and
`on_low_e` latch on the downward crossing and unlatch on the upward
crossing.

Without latching, `on_full` fires on every refill when the ring sits at
capacity — millions of times per second in a saturation test. Latching
reduces it to once per drain-refill episode. That is the signal you actually
want: *"we are now full — stop reading the socket."*

`on_slot_added` is not latched because it is a per-item metric, not a
threshold event.

The consumer path is callback-free on the hot loop. `rb_drain` never
consults a callback.

---

## Configuration

### Compile-time (changes ABI / struct layout / hot path)

These are set via `-D` flags or CMake options. They affect struct layout,
entry type, or which code paths exist.

| Macro                       | Default  | Meaning                                                        |
|---------------------------|--------|--------------------------------------------------------------|
| `RB_SLOT_SIZE`              | `2048`   | Bytes per slot, incl. header (4 B; 8 B with RB_PER_SLOT_LAP=1) |
| `RB_NUM_SLOTS`              | `64`     | Default scratchpad slot count                                  |
| `RB_CAPACITY`               | `64`     | Default ring capacity (power of two)                           |
| `RB_CACHE_LINE`             | `64`     | Cache-line size for padding/alignment                          |
| `RB_USE_POINTERS`           | `0`      | 1: entries are `void*`, 0: `uint32_t`                          |
| `RB_SLOT_CACHELINE_PAD`     | `0`      | 1: pad slot stride to a cache line                             |
| `RB_SINGLE_THREADED`        | `0`      | 1: no atomics, no barriers                                     |
| `RB_USE_ATOMICS`            | `1`      | 1: C11 `<stdatomic.h>`, 0: porting barriers                    |
| `RB_PER_SLOT_LAP`           | `0`      | 1: per-slot lap mode (8-byte slot header)                      |
| `RB_ENABLE_STATS`           | `1`      | 1: compile in counters and watermarks                          |
| `RB_ENABLE_THREAD_HELPERS`  | `1`      | 1: build pthread spawn/join helper                             |
| `RB_ENABLE_NOTIFY`          | `0`      | 1: build eventfd/pipe wake-up helpers                          |
| `RB_DEFAULT_LOW_D`          | `25`     | Default low_d percentage                                       |
| `RB_DEFAULT_LOW_E`          | `10`     | Default low_e percentage                                       |

`RB_SINGLE_THREADED=1` forces `RB_USE_ATOMICS=0` and makes the ring
compile down to a plain circular FIFO with a mask. No barriers, no atomic
loads, works from an ISR or main loop.

### Runtime (policy only, no layout change)

Every size, threshold, callback, and policy is settable via `rb_config_t`
before init, and most via `rb_set_*` after init.

| Setter                          | Changeable after init?             |
|---------------------------------|------------------------------------|
| `rb_set_limit`                  | yes, but only while ring is empty  |
| `rb_set_low_d` / `rb_set_low_e` | yes                                |
| `rb_set_oversize_policy`        | yes                                |
| `rb_set_callbacks`              | yes                                |
| `rb_set_producer_stack`         | yes, before `rb_thread_spawn`      |
| `rb_set_consumer_stack`         | yes, before `rb_thread_spawn`      |

Capacity, slot count, and slot size are frozen at init. Changing them
would invalidate the entries and the scratchpad layout.

### Oversize handling

When `wanted_len > slot_size - 4`:

- `RB_OVERSIZE_TRUNCATE` (default): write `cap` bytes, set the TRUNCATED
  flag, publish. Consumer sees the flag and decides what to do.
- `RB_OVERSIZE_DROP`: return `RB_ERR_OVERSIZE`, do not publish, slot
  stays free.

The producer **never** writes past the slot boundary. That is a hard
invariant.

---

## Threading models

The ring itself is thread-agnostic. It is shared state with atomics. How
you drive it is your choice.

### 1. Single-threaded event loop (recommended default)

Both producer and consumer run in the same loop. No atomics, no barriers,
no threads. This is what you want for a WS transform pipeline where the
socket reader and the transformer live in the same process and the same
event loop.

```c
/* on socket readable */
rb_err_t e = rb_acquire(rb, frame_len, &idx, &w, &cap);
if (e == RB_OK) {
    ssize_t n = read(fd, w, cap);
    if (n > 0) rb_publish(rb, idx, (uint32_t)n);
    else       rb_abort(rb);
} else if (e == RB_ERR_FULL) {
    /* stop reading; backpressure applied via TCP window */
}

/* at end of loop tick */
rb_drain(rb, transform_cb, user);
```

Build with `-DRB_SINGLE_THREADED=ON` for this mode.

### 2. Cross-thread SPSC

Producer in one thread, consumer in another. Atomics required. Optional
`rb_notify` gives you eventfd-based wake-ups between them.

Build with default flags, spawn via `rb_thread_spawn` with configurable
stack sizes, or drive each side from its own loop.

### 3. Embedded / no-OS

Producer is an ISR or a hardware callback, consumer is the main loop.
Build with `-DRB_SINGLE_THREADED=ON -DRB_ENABLE_THREAD_HELPERS=OFF
-DRB_ENABLE_NOTIFY=OFF`. The library compiles to a few hundred bytes of
code with no external dependencies.

---

## Backpressure pattern

The intended usage:

- `on_full` fires → producer callback sets `stop_reading = true`.
- `on_low_d` fires → producer callback clears it.
- The event loop checks `stop_reading` on each iteration.
- The library never stalls the producer itself. It only reports.

The consumer runs at full speed via `rb_drain`. It does not consult
thresholds, does not wait, returns as soon as the ring is empty.

The hysteresis band between `low_d` and 100% is your safety margin. A
wider band means the producer gets more warning before saturation; a
narrower band means less idle time.

---

## Public API

```c
/* Config */
void     rb_config_init(rb_config_t *cfg);
void     rb_config_set_capacity(rb_config_t *, uint32_t);
void     rb_config_set_limit(rb_config_t *, uint32_t);
void     rb_config_set_slots(rb_config_t *, uint32_t);
void     rb_config_set_slot_size(rb_config_t *, uint32_t);
void     rb_config_set_low_d(rb_config_t *, uint32_t);
void     rb_config_set_low_e(rb_config_t *, uint32_t);
void     rb_config_set_oversize_policy(rb_config_t *, rb_oversize_policy_t);
void     rb_config_set_callbacks(rb_config_t *, const rb_callbacks_t *);
void     rb_config_set_producer_stack(rb_config_t *, size_t);
void     rb_config_set_consumer_stack(rb_config_t *, size_t);

/* Lifecycle */
size_t   rb_size(uint32_t capacity);
rb_err_t rb_init(rb_t *, const rb_config_t *, void *scratch, size_t scratch_size);
void     rb_deinit(rb_t *);

/* Producer */
rb_err_t rb_acquire(rb_t *, uint32_t wanted_len,
                    uint32_t *out_idx, void **out_w, uint32_t *out_cap);
rb_err_t rb_publish(rb_t *, uint32_t idx, uint32_t len);
rb_err_t rb_publish_ex(rb_t *, uint32_t idx, uint32_t len, bool truncated);
rb_err_t rb_abort(rb_t *);

/* Consumer */
rb_err_t rb_consume(rb_t *, uint32_t *out_idx, const void **out_obj,
                    uint32_t *out_len, bool *out_truncated);
rb_err_t rb_release(rb_t *, uint32_t idx);

/* Batch */
uint32_t rb_drain(rb_t *, rb_drain_fn, void *user);
uint32_t rb_flush(rb_t *, rb_flush_fn, void *user);

/* Introspection */
uint32_t rb_count(const rb_t *);
uint32_t rb_capacity(const rb_t *);
uint32_t rb_limit(const rb_t *);
bool     rb_is_full(const rb_t *);
bool     rb_is_empty(const rb_t *);
void    *rb_slot_ptr(rb_t *, uint32_t idx);
const void *rb_slot_cptr(const rb_t *, uint32_t idx);
size_t   rb_producer_stack(const rb_t *);
size_t   rb_consumer_stack(const rb_t *);

/* Runtime reconfig */
rb_err_t rb_set_limit(rb_t *, uint32_t);
rb_err_t rb_set_low_d(rb_t *, uint32_t);
rb_err_t rb_set_low_e(rb_t *, uint32_t);
rb_err_t rb_set_oversize_policy(rb_t *, rb_oversize_policy_t);
rb_err_t rb_set_callbacks(rb_t *, const rb_callbacks_t *);
rb_err_t rb_set_producer_stack(rb_t *, size_t);
rb_err_t rb_set_consumer_stack(rb_t *, size_t);

#if RB_ENABLE_STATS
const rb_stats_t *rb_stats(const rb_t *);
void              rb_stats_reset(rb_t *);
#endif
```

`rb_size` returns the byte size of the control block. The caller allocates
it and a scratchpad, both aligned to `RB_CACHE_LINE`. No `malloc` inside
the library.

---

## Building

### Native, with tests

```bash
cmake -B build -DRB_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### All variants (default, pointer, cachepad, TSan, ASan)

```bash
./run-all-checks.sh
```

### Single-threaded / bare-metal shape

```bash
cmake -B build-st \
  -DRB_SINGLE_THREADED=ON \
  -DRB_ENABLE_THREAD_HELPERS=OFF \
  -DRB_ENABLE_NOTIFY=OFF \
  -DRB_ENABLE_STATS=OFF
```

### Cross-compile to ARM Linux

```bash
sudo apt install gcc-arm-linux-gnueabihf
cmake -B build-arm \
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm
file build-arm/librb.a      # should say: ELF 32-bit LSB, ARM, EABI5
```

### Cross-compile to Android arm64

```bash
cmake -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=cmake/android-arm64.cmake \
  -DANDROID_NDK=/path/to/android-ndk \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android
```

### Benchmarks

```bash
cmake -B build-bench -DRB_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench
./build-bench/bench/bench_rb
./build-bench/bench/bench_rb_random
```

---

## Testing

| Test                  | What it proves                                              |
|-----------------------|-------------------------------------------------------------|
| `test_rb`             | Unit tests: init, lifecycle, producer/consumer, truncation, drop policy, latched callbacks, drain, flush, runtime setters, stats, mis-pairing rejection |
| `test_rb_threaded`    | 2M-item SPSC across threads: no loss, no dupes, strict FIFO, no payload corruption |
| `test_rb_backpressure`| Fast producer / slow consumer: ring saturates, producer throttles, no loss |

Sanitizer builds:

- **TSan** — proves the release/acquire ordering on `head` and `tail` is correct, and that `full_latch` is atomic (it is).
- **ASan + UBSan** — proves the manual stride arithmetic and pointer recovery from entries are sound.

Both are run by `run-all-checks.sh`.

---

## Benchmarks

Baseline on x86_64 (single-threaded cycle = acquire + publish + consume + release):

```
capacity=64:
  slot=64     cap=64        38.11 M cycles/s  (  26.24 ns / cycle)
  slot=256    cap=64        41.51 M cycles/s  (  24.09 ns / cycle)
  slot=2048   cap=64        44.15 M cycles/s  (  22.65 ns / cycle)
  slot=8192   cap=64        41.97 M cycles/s  (  23.83 ns / cycle)

capacity=1024:
  slot=64     cap=1024      42.97 M cycles/s  (  23.27 ns / cycle)
  slot=256    cap=1024      41.82 M cycles/s  (  23.91 ns / cycle)
  slot=2048   cap=1024      44.09 M cycles/s  (  22.68 ns / cycle)
  slot=8192   cap=1024      25.56 M cycles/s  (  39.13 ns / cycle)  <- L2 miss
```

Takeaways:

- 22 ns per full cycle is a fast baseline — roughly one cache miss plus
  the header `memcpy`.
- 2 KB slots at 64 capacity (your WS shape) are the fastest row. Scaling
  capacity from 64 to 1024 costs nothing at this slot size.
- The last row is the useful warning: when `capacity * stride` exceeds
  L2 (~8 MB working set), throughput drops ~40%. Budget accordingly if
  you ever raise capacity with large slots.

The `bench_rb_random` benchmark repeats this with variable-length payloads
filled from a PRNG, to model real WS frames rather than fixed-size items.

---

## Porting

The library has one porting header, `rb_port.h`, that abstracts:

- **Atomics** — C11 `<stdatomic.h>` when available, otherwise compiler
  barriers or plain reads/writes depending on `RB_SINGLE_THREADED`.
- **Alignment** — `_Alignas` when available, `__attribute__((aligned))`
  otherwise.
- **Cache line** — overridable via `RB_CACHE_LINE`.

To port to a new architecture, provide:

- a working C11 compiler (or a C99 compiler plus a barriers fallback),
- `RB_CACHE_LINE` matching the target's cache line,
- an atomics implementation if `<stdatomic.h>` is not available.

No POSIX or Linux headers are used in `rb.c`. `rb_thread.c` and
`rb_notify.c` do use POSIX, but they are gated behind
`RB_ENABLE_THREAD_HELPERS` and `RB_ENABLE_NOTIFY`, both off by default
for embedded builds.

---

## Design decisions

Why the choices that are not obvious:

- **Indices instead of pointers by default.** Smaller entries (4 vs 8
  bytes), relocatable if the scratchpad base moves, bounds-checkable,
  safe for shared memory later. Pointers available via
  `RB_USE_POINTERS=1` for the cases where you want to do arithmetic on
  them.

- **Strict FIFO release order.** The consumer releases slots in the same
  order it consumed them. This removes the need for a free-list ring —
  the producer simply takes `head % slots` at acquire time. If you ever
  need out-of-order release, you would add a free-list ring; not done
  now because the WS use case is strictly FIFO.

- **Latched threshold callbacks.** Without latching, `on_full` fires on
  every refill when the ring is saturated. That is not a useful
  backpressure signal; it is a per-operation counter. Latching turns it
  into a per-episode notification, which is what the producer's
  `stop_reading` flag actually wants.

- **Truncation is a flag, never an error.** Under the default policy,
  an oversize write produces a slot with the TRUNCATED bit set. The
  library never drops the payload silently and never lets the producer
  write past the slot boundary. The consumer sees the flag and decides.

- **Callbacks are informational, never gating.** This is what makes the
  consumer path callback-free and lets `rb_drain` run at full speed.
  Threshold callbacks fire as side effects of `rb_publish` and
  `rb_release`; the return value of `rb_publish` is the only thing a
  producer branches on.

- **`rb_init` takes a caller-owned scratchpad.** No `malloc` in the core.
  The caller can place the scratchpad in a static buffer, an mmap, or
  shared memory. `rb_size()` returns the control block size so the
  caller can do the same for it.

- **`RB_SINGLE_THREADED` exists.** For embedded, atomics and barriers
  are pure overhead. Flipping one macro makes them vanish.

---

## Non-goals

- **MPSC / MPMC.** The ring is strictly SPSC. Multiple producers or
  multiple consumers are not supported and will race.
- **Dynamic resizing.** Capacity, slot count and slot size are frozen
  at init. `limit` can change while empty; nothing else.
- **Owning a thread or a loop.** The library never spawns and never
  drives a loop. It is a data structure. `rb_thread` and `rb_notify`
  are optional helpers; they do not make the library own anything.
- **Variable-size slots / arena allocation.** Every slot is the same
  stride. If you need size classes, layer them on top (multiple rings).
- **Protocol parsing.** No WS framing, no HTTP, no nothing. Just the
  queue and the scratchpad.

---

## License

Personal use. Add your license of choice here.
