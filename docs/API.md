# rb — Complete API reference

[← Back to overview](../README.md) · [Technical notes](TECHNICAL.md)

Every public function, type, macro, and behaviour of the `rb` library,
with runnable examples.

---

## Table of contents

- [Headers](#headers)
- [Types](#types)
- [Macros](#macros)
- [Lifecycle](#lifecycle)
- [Configuration](#configuration)
- [Producer API](#producer-api)
- [Consumer API](#consumer-api)
- [Batch API](#batch-api)
- [Introspection](#introspection)
- [Runtime reconfiguration](#runtime-reconfiguration)
- [Stats](#stats)
- [Thread helpers (optional)](#thread-helpers-optional)
- [Notify helpers (optional)](#notify-helpers-optional)
- [Error codes](#error-codes)
- [Complete worked example](#complete-worked-example)

---

## Headers

```c
#include "rb.h"          // always (includes rb_config.h)
#include "rb_thread.h"   // if RB_ENABLE_THREAD_HELPERS
#include "rb_notify.h"   // optional compatibility shim (includes rb.h)
```

`rb.h` transitively includes `rb_config.h`. It has `extern "C"` guards,
so it is safe to include from C++.

Notification support (`rb_wait`, `rb_notify_value`, `rb_notify_fd`, …)
lives **inside** `rb.h` / `rb.c` when built with `RB_ENABLE_NOTIFY=1`.
`rb_notify.h` is only a thin compatibility include that pulls in `rb.h`;
new code does not need it.

---

## Types

### `rb_t`

```c
typedef struct rb_s rb_t;
```

Opaque handle. The struct definition lives in `src/rb.c` and is never
visible to callers. You allocate the storage; the library manages its
contents.

The storage must be aligned to `_Alignof(rb_t)`. In practice this equals
`RB_CACHE_LINE`, so aligning to that is always correct.

### `rb_err_t`

```c
typedef enum {
    RB_OK = 0,
    RB_ERR_FULL,
    RB_ERR_EMPTY,
    RB_ERR_OVERSIZE,
    RB_ERR_INVAL,
    RB_ERR_NOT_INIT
} rb_err_t;
```

See [Error codes](#error-codes) for the full table.

### `rb_entry_t`

```c
#if RB_USE_POINTERS
typedef void *rb_entry_t;
#else
typedef uint32_t rb_entry_t;
#endif
```

The type stored in the ring. Default is a `uint32_t` slot index; with
`RB_USE_POINTERS=1` it is a `void *` pointing at the slot.

### `rb_oversize_policy_t`

```c
typedef enum {
    RB_OVERSIZE_TRUNCATE = 0,   /* write cap bytes, flag TRUNCATED, publish */
    RB_OVERSIZE_DROP     = 1    /* return RB_ERR_OVERSIZE, do not publish */
} rb_oversize_policy_t;
```

### `rb_config_t`

```c
typedef struct {
    uint32_t capacity;           /* power of two; 0 -> RB_CAPACITY */
    uint32_t limit;              /* logical max; 0 -> capacity */
    uint32_t slots;              /* scratchpad slots; 0 -> RB_NUM_SLOTS */
    uint32_t slot_size;          /* bytes per slot incl. header */
    uint32_t low_d;              /* percent 0..100 */
    uint32_t low_e;              /* percent 0..100, must be <= low_d */
    rb_oversize_policy_t oversize_policy;
    size_t   producer_stack_size; /* 0 -> RB_DEFAULT_PRODUCER_STACK */
    size_t   consumer_stack_size; /* 0 -> RB_DEFAULT_CONSUMER_STACK */
    rb_callbacks_t cb;
} rb_config_t;
```

Initialise with `rb_config_init` before setting fields. Setting a field
to `0` means "use the compile-time default".

### `rb_callbacks_t`

```c
typedef void (*rb_cb_t)(rb_t *rb, void *user);
typedef void (*rb_slot_cb_t)(rb_t *rb, uint32_t slot_index,
                             uint32_t len, bool truncated, void *user);

typedef struct {
    rb_slot_cb_t on_slot_added;
    rb_cb_t      on_low_d;
    rb_cb_t      on_low_e;
    rb_cb_t      on_full;
    void        *user;
} rb_callbacks_t;
```

All four are informational. They must be non-blocking and must not
re-enter the ring. See [Callback semantics](TECHNICAL.md#callback-semantics)
for latching behaviour.

### `rb_stats_t`

```c
#if RB_ENABLE_STATS
typedef struct {
    uint64_t published;
    uint64_t consumed;
    uint64_t truncated;
    uint64_t full_attempts;
    uint32_t high_water;
    uint32_t full_hits;
    uint32_t low_d_hits;
    uint32_t low_e_hits;
} rb_stats_t;
#endif
```

### Function pointer types

```c
typedef bool (*rb_drain_fn)(const void *obj, uint32_t len,
                            bool truncated, void *user);

typedef bool (*rb_flush_fn)(void *writable, uint32_t cap,
                            uint32_t *out_len, bool *out_truncated,
                            bool *out_stop, void *user);
```

---

## Macros

### Slot header

```c
#define RB_SLOT_TRUNCATED 0x80000000u
#define RB_SLOT_LEN_MASK  0x7FFFFFFFu
#define RB_SLOT_HDR_SIZE  4u   /* 8u when RB_PER_SLOT_LAP = 1 */
```

Every slot begins with a header: 4 bytes in Mode 0 (default), 8 bytes
in Mode 1 (`RB_PER_SLOT_LAP=1`). Bits 0–30 hold the payload length;
bit 31 is the `TRUNCATED` flag. In Mode 1 the header also carries the
per-slot sequence number (see [TECHNICAL.md](TECHNICAL.md#two-execution-modes)).
Effective payload capacity per slot is `slot_size - RB_SLOT_HDR_SIZE`.

### Compile-time configuration

See [Configuration](TECHNICAL.md#configuration) in the technical notes.
The ones that affect the public API's behaviour:

| Macro | Default | Effect |
|---|---|---|
| `RB_SLOT_SIZE` | `2048` | Bytes per slot, incl. header |
| `RB_NUM_SLOTS` | `64` | Default scratchpad slot count |
| `RB_CAPACITY` | `64` | Default ring capacity |
| `RB_CACHE_LINE` | `64` | Alignment and padding unit |
| `RB_USE_POINTERS` | `0` | 1 = `void*` entries, 0 = `uint32_t` |
| `RB_SLOT_CACHELINE_PAD` | `0` | 1 = pad slot stride to cache line |
| `RB_SINGLE_THREADED` | `0` | 1 = no atomics, no barriers |
| `RB_USE_ATOMICS` | `1` | 1 = C11 `<stdatomic.h>` |
| `RB_PER_SLOT_LAP` | `0` | 1 = Vyukov per-slot-lap mode (8-byte slot header) |
| `RB_ENABLE_STATS` | `1` | 1 = compile in counters |
| `RB_ENABLE_THREAD_HELPERS` | `1` | 1 = build `rb_thread.c` |
| `RB_ENABLE_NOTIFY` | `0` | 1 = futex + eventfd wake-up helpers in `rb.c` |
| `RB_DEFAULT_LOW_D` | `25u` | Default low_d percentage |
| `RB_DEFAULT_LOW_E` | `10u` | Default low_e percentage |
| `RB_FUTEX_SHARED` | `0` | Process-shared futex for wake-up (cleared by default) |
| `RB_NOTIFY_WAIT_SLICE_MS` | `100u` | Notify wait slice, in ms |
| `RB_DEFAULT_PRODUCER_STACK` | `(256u * 1024u)` | Default producer thread stack size in bytes |
| `RB_DEFAULT_CONSUMER_STACK` | `(256u * 1024u)` | Default consumer thread stack size in bytes |
| `RB_THREAD_MIN_STACK` | `(64u * 1024u)` | Minimum allowed thread stack size in bytes |
| `RB_MEMCPY` / `RB_MEMSET`* | `memcpy` / `memset` | Memory copy helpers (in `rb_port.h`) |
| `RB_PREFETCH_R` / `RB_PREFETCH_W`* | `__builtin_prefetch` / no-op | Prefetch helpers (in `rb_port.h`) |

\* Portability hooks defined in `rb_port.h`, not `rb_config.h`; see [Dependencies](DEPENDENCIES.md) and [Changelog](CHANGELOG.md).

---

## Lifecycle

### `rb_size`

```c
size_t rb_size(uint32_t capacity);
```

Returns the byte size of the control block for a given `capacity`. Use
it to allocate the control block. Align the result to `_Alignof(rb_t)`
(or `RB_CACHE_LINE`).

**Example:**

```c
size_t bytes = rb_size(256);
void *mem = aligned_alloc_rb(bytes);
rb_t *rb = (rb_t *)mem;
```

### `rb_init`

```c
rb_err_t rb_init(rb_t *rb, const rb_config_t *cfg,
                 void *scratch, size_t scratch_size);
```

Initialise the ring. Both `rb` and `scratch` must be caller-owned. The
scratchpad must be at least `slots * stride` bytes, where `stride` is
`slot_size` rounded up to `RB_CACHE_LINE` (if `RB_SLOT_CACHELINE_PAD=1`)
or to `_Alignof(max_align_t)` otherwise. Both buffers must be aligned to
the same unit.

**Validation performed:**

- `capacity` must be a power of two and ≥ 2
- `slots` must be nonzero
- `slot_size` must be > `RB_SLOT_HDR_SIZE`
- `limit` (or default) is clamped to `min(capacity, slots)` and must be > 0
- `low_d` and `low_e` must be ≤ 100, with `low_e ≤ low_d`
- `scratch` and `rb` must be suitably aligned
- `scratch_size` must be large enough

**Returns:** `RB_OK` on success, `RB_ERR_INVAL` on any validation failure.

**Example:**

```c
rb_config_t cfg;
rb_config_init(&cfg);
cfg.capacity  = 256;
cfg.slots     = 256;
cfg.slot_size = 2048;

rb_t *rb = aligned_alloc_rb(rb_size(256));
void *scratch = aligned_alloc_rb(256 * 2048);

if (rb_init(rb, &cfg, scratch, 256 * 2048) != RB_OK) {
    /* handle */
}
```

### `rb_deinit`

```c
void rb_deinit(rb_t *rb);
```

Release the library's reference to the scratchpad. Does not free the
control block or the scratchpad — both are caller-owned. Safe to call
with `NULL`.

**Example:**

```c
rb_deinit(rb);
free(scratch);
free(rb);
```

---

## Configuration

All `rb_config_set_*` functions are one-liners that set a field on an
already-initialised `rb_config_t`. Safe to call with `NULL` (no-op).

```c
void rb_config_init(rb_config_t *cfg);
void rb_config_set_capacity(rb_config_t *cfg, uint32_t capacity);
void rb_config_set_limit(rb_config_t *cfg, uint32_t limit);
void rb_config_set_slots(rb_config_t *cfg, uint32_t slots);
void rb_config_set_slot_size(rb_config_t *cfg, uint32_t slot_size);
void rb_config_set_low_d(rb_config_t *cfg, uint32_t percent);
void rb_config_set_low_e(rb_config_t *cfg, uint32_t percent);
void rb_config_set_oversize_policy(rb_config_t *cfg, rb_oversize_policy_t p);
void rb_config_set_callbacks(rb_config_t *cfg, const rb_callbacks_t *cb);
void rb_config_set_producer_stack(rb_config_t *cfg, size_t bytes);
void rb_config_set_consumer_stack(rb_config_t *cfg, size_t bytes);
```

`rb_config_init` sets defaults from the compile-time macros and zeroes
everything else. **Always call it before setting fields.**

**Example — all fields:**

```c
rb_config_t cfg;
rb_config_init(&cfg);

rb_config_set_capacity(&cfg, 512);
rb_config_set_limit(&cfg, 256);        /* logical max below capacity */
rb_config_set_slots(&cfg, 512);
rb_config_set_slot_size(&cfg, 4096);
rb_config_set_low_d(&cfg, 40);
rb_config_set_low_e(&cfg, 15);
rb_config_set_oversize_policy(&cfg, RB_OVERSIZE_DROP);
rb_config_set_producer_stack(&cfg, 512 * 1024);
rb_config_set_consumer_stack(&cfg, 512 * 1024);

rb_callbacks_t cb = {0};
cb.on_full  = on_full_cb;
cb.on_low_d = on_low_d_cb;
cb.user     = &my_context;
rb_config_set_callbacks(&cfg, &cb);
```

---

## Producer API

### `rb_acquire`

```c
rb_err_t rb_acquire(rb_t *rb, uint32_t wanted_len,
                    uint32_t *out_slot_index,
                    void **out_writable, uint32_t *out_cap);
```

Reserve a slot for writing. On success, `*out_writable` points **past**
the 4-byte header; the caller writes up to `*out_cap` bytes there.

**Parameters:**

- `wanted_len` — caller's intended payload size. Used only for the
  oversize check. Pass `0` to skip the check (used by `rb_flush`).
- `out_slot_index` — receives the slot index. Required for the matching
  `rb_publish` / `rb_abort` call. May be `NULL` if you don't need it.
- `out_writable` — receives the writable pointer.
- `out_cap` — receives the writable capacity, `slot_size - 4`.

**Returns:**

- `RB_OK` — slot reserved
- `RB_ERR_FULL` — ring is at `limit`
- `RB_ERR_OVERSIZE` — `wanted_len > cap` and policy is `DROP`
- `RB_ERR_INVAL` — a previous acquire is still pending
- `RB_ERR_NOT_INIT` — `rb` is NULL or uninitialised

**Contract:** exactly one `rb_acquire` must be followed by exactly one
`rb_publish`, `rb_publish_ex`, or `rb_abort`, from the same thread, before
the next `rb_acquire`.

**Example:**

```c
uint32_t idx, cap;
void *w;
rb_err_t e = rb_acquire(rb, 1024, &idx, &w, &cap);
if (e == RB_OK) {
    ssize_t n = read(fd, w, cap);
    if (n > 0) rb_publish(rb, idx, (uint32_t)n);
    else       rb_abort(rb);
}
```

### `rb_publish`

```c
rb_err_t rb_publish(rb_t *rb, uint32_t slot_index, uint32_t written_len);
```

Publish a slot written by `rb_acquire`. Makes the slot visible to the
consumer. Fires `on_slot_added` and possibly `on_full`. Updates stats.

If `written_len > slot_size - RB_SLOT_HDR_SIZE`, it is clamped and the
`TRUNCATED` flag is set.

**Returns:**

- `RB_OK`
- `RB_ERR_INVAL` — no acquire pending, or `slot_index` mismatch
- `RB_ERR_NOT_INIT`

**Example:**

```c
memcpy(w, "hello", 5);
rb_publish(rb, idx, 5);
```

### `rb_publish_ex`

```c
rb_err_t rb_publish_ex(rb_t *rb, uint32_t slot_index,
                       uint32_t written_len, bool truncated);
```

Identical to `rb_publish`, but lets the caller set the `TRUNCATED` flag
explicitly. Useful when the caller knows the frame was intentionally cut
short for reasons unrelated to slot capacity.

**Example:**

```c
/* Forward only the first 512 bytes of a 4 KB frame. */
memcpy(w, frame, 512);
rb_publish_ex(rb, idx, 512, true);   /* flag it as truncated */
```

### `rb_abort`

```c
rb_err_t rb_abort(rb_t *rb);
```

Cancel an in-flight `rb_acquire`. The reserved slot is returned to the
producer without publishing.

**Returns:**

- `RB_OK`
- `RB_ERR_INVAL` — no acquire pending
- `RB_ERR_NOT_INIT`

**Example:**

```c
rb_acquire(rb, 100, &idx, &w, &cap);
if (some_error_condition) {
    rb_abort(rb);
} else {
    /* fill w, then */ rb_publish(rb, idx, n);
}
```

---

## Consumer API

### `rb_consume`

```c
rb_err_t rb_consume(rb_t *rb, uint32_t *out_slot_index,
                    const void **out_obj, uint32_t *out_len,
                    bool *out_truncated);
```

Pop the oldest published slot. On success, `*out_obj` points past the
header, `*out_len` is the payload length, and `*out_truncated` reflects
the header flag.

**Returns:**

- `RB_OK`
- `RB_ERR_EMPTY` — nothing available
- `RB_ERR_INVAL` — a previous consume is still pending
- `RB_ERR_NOT_INIT`

**Contract:** exactly one `rb_consume` must be followed by exactly one
`rb_release`, from the same thread, before the next `rb_consume`.

**Example:**

```c
uint32_t idx, len;
const void *obj;
bool trunc;
if (rb_consume(rb, &idx, &obj, &len, &trunc) == RB_OK) {
    if (!trunc) {
        process(obj, len);
    }
    rb_release(rb, idx);
}
```

### `rb_release`

```c
rb_err_t rb_release(rb_t *rb, uint32_t slot_index);
```

Return the slot to the producer. Advances the tail, fires `on_low_d` and
`on_low_e` if the corresponding thresholds were crossed, updates stats.

**Returns:**

- `RB_OK`
- `RB_ERR_INVAL` — no consume pending, or `slot_index` mismatch
- `RB_ERR_NOT_INIT`

**Example:** see `rb_consume`.

---

## Batch API

### `rb_drain`

```c
uint32_t rb_drain(rb_t *rb, rb_drain_fn fn, void *user);
```

Consume every available slot, calling `fn` on each. Releases each slot
before consuming the next. Does **not** consult any callback. Returns as
soon as the ring is empty.

`fn` receives the payload pointer, length, truncation flag, and `user`.
Return `true` to continue, `false` to stop after the current slot.

**Returns:** the number of slots processed.

**Example:**

```c
static bool on_frame(const void *obj, uint32_t len,
                     bool trunc, void *user) {
    if (trunc) return true;      /* skip, keep draining */
    process_frame((const uint8_t *)obj, len);
    return true;                 /* keep going */
}

void loop_tick(rb_t *rb) {
    uint32_t n = rb_drain(rb, on_frame, NULL);
    /* n = frames processed this tick */
}
```

**Fairness variant** — stop after a batch to keep the loop responsive:

```c
typedef struct { uint32_t budget; } batch_ctx;

static bool batch_fn(const void *obj, uint32_t len,
                     bool trunc, void *user) {
    batch_ctx *b = user;
    if (!trunc) process(obj, len);
    if (b->budget == 0) return false;   /* stop early */
    b->budget--;
    return true;
}

void loop_tick(rb_t *rb) {
    batch_ctx b = { .budget = 32 };
    rb_drain(rb, batch_fn, &b);
}
```

### `rb_flush`

```c
uint32_t rb_flush(rb_t *rb, rb_flush_fn fn, void *user);
```

Publish slots until the ring is full or the callback says to stop.
Symmetric to `rb_drain`.

`fn` receives a writable region and its capacity. It must fill the
region, set `*out_len`, optionally set `*out_truncated`, and optionally
set `*out_stop = true` to end the flush after this slot. Return `false`
to abort without publishing.

**Returns:** the number of slots published.

**Example:**

```c
static bool fill_from_queue(void *w, uint32_t cap, uint32_t *out_len,
                            bool *out_trunc, bool *out_stop, void *user) {
    frame_source *src = user;
    frame f;
    if (!source_pop(src, &f)) { *out_stop = true; return true; }
    uint32_t n = f.len < cap ? f.len : cap;
    memcpy(w, f.data, n);
    *out_len     = n;
    *out_trunc   = (f.len > cap);
    *out_stop    = false;
    return true;
}

void loop_tick(rb_t *rb) {
    rb_flush(rb, fill_from_queue, &my_source);
}
```

---

## Introspection

### `rb_count`

```c
uint32_t rb_count(const rb_t *rb);
```

Number of entries currently in the ring. On a cross-thread ring this is
a snapshot; the true value may change immediately after the call.

### `rb_capacity`

```c
uint32_t rb_capacity(const rb_t *rb);
```

The physical ring capacity set at init. Never changes.

### `rb_limit`

```c
uint32_t rb_limit(const rb_t *rb);
```

The logical maximum set at init or via `rb_set_limit`. Producers stop
at this count, which may be less than `capacity`.

### `rb_is_full` / `rb_is_empty`

```c
bool rb_is_full(const rb_t *rb);
bool rb_is_empty(const rb_t *rb);
```

Convenience wrappers around `rb_count`.

### `rb_slot_ptr` / `rb_slot_cptr`

```c
void       *rb_slot_ptr (rb_t *rb, uint32_t slot_index);
const void *rb_slot_cptr(const rb_t *rb, uint32_t slot_index);
```

Direct access to a slot's payload region, past the header. **Not** the
normal path — you should use `rb_acquire` / `rb_consume`. Provided for
debugging and for the rare case where you want to peek at a slot's
contents without going through the queue.

Returns `NULL` if `slot_index >= slots`.

### `rb_producer_stack` / `rb_consumer_stack`

```c
size_t rb_producer_stack(const rb_t *rb);
size_t rb_consumer_stack(const rb_t *rb);
```

The stack sizes that `rb_thread_spawn` will use. Read-only.

---

## Runtime reconfiguration

### `rb_set_limit`

```c
rb_err_t rb_set_limit(rb_t *rb, uint32_t limit);
```

Change the logical maximum. **Only safe when the ring is empty.**
Returns `RB_ERR_INVAL` if `rb_count(rb) != 0`, or if `limit` is 0 or
exceeds `min(capacity, slots)`.

### `rb_set_low_d` / `rb_set_low_e`

```c
rb_err_t rb_set_low_d(rb_t *rb, uint32_t percent);
rb_err_t rb_set_low_e(rb_t *rb, uint32_t percent);
```

Change the low-water thresholds as percentages of `limit`. Safe to call
at any time. Takes effect on the next `rb_release`.

**Note:** `rb_set_low_e` does not enforce `low_e ≤ low_d`. Passing
`low_e > low_d` produces a configuration where `on_low_e` fires before
`on_low_d`, which is legal but unusual.

### `rb_set_oversize_policy`

```c
rb_err_t rb_set_oversize_policy(rb_t *rb, rb_oversize_policy_t p);
```

Switch between `TRUNCATE` and `DROP`. Takes effect on the next
`rb_acquire`.

### `rb_set_callbacks`

```c
rb_err_t rb_set_callbacks(rb_t *rb, const rb_callbacks_t *cb);
```

Replace the callback set. Safe at any time; takes effect on the next
event that would fire a callback.

### `rb_set_producer_stack` / `rb_set_consumer_stack`

```c
rb_err_t rb_set_producer_stack(rb_t *rb, size_t bytes);
rb_err_t rb_set_consumer_stack(rb_t *rb, size_t bytes);
```

Change the stack sizes used by subsequent `rb_thread_spawn` calls.

---

## Stats

Available when `RB_ENABLE_STATS=1` (default).

```c
const rb_stats_t *rb_stats(const rb_t *rb);
void rb_stats_reset(rb_t *rb);
```

`rb_stats` returns a pointer to the live counters (or `NULL` if stats
are disabled / `rb` is `NULL`). `rb_stats_reset` zeroes them.

---

## Thread helpers (optional)

Available when `RB_ENABLE_THREAD_HELPERS=1` (default). Included via
`#include "rb_thread.h"`.

### Types

```c
typedef pthread_t rb_thread_t;
typedef void *(*rb_thread_fn)(void *arg);
```

### `rb_thread_spawn`

```c
rb_err_t rb_thread_spawn(rb_t *rb, rb_thread_t *out, bool is_producer,
                         rb_thread_fn fn, void *arg);
```

Spawn a thread using the stack size from the ring's config or from
`rb_set_*_stack`. `is_producer` selects which stack size applies.

Internally:

- stack size is clamped to at least `RB_THREAD_MIN_STACK`
- rounded up to the system page size
- set via `pthread_attr_setstacksize`

**Returns:**

- `RB_OK`
- `RB_ERR_INVAL` — bad arguments
- `RB_ERR_NOT_INIT` — ring not initialised

**Example:**

```c
rb_thread_t tp, tc;
rb_thread_spawn(rb, &tp, true,  producer_fn, &ctx);
rb_thread_spawn(rb, &tc, false, consumer_fn, &ctx);

rb_thread_join(&tp);
rb_thread_join(&tc);
```

### `rb_thread_join`

```c
rb_err_t rb_thread_join(rb_thread_t *t);
```

Join a spawned thread. Returns `RB_OK` on success.

---

## Notify helpers (optional)

Available when `RB_ENABLE_NOTIFY=1` **and** the target is Linux (or
Android). The functions are declared in `rb.h` and implemented in
`rb.c`. There is no separate notification object.

- Header default (`rb_config.h`): `RB_ENABLE_NOTIFY=0` (off).
- CMake default: `RB_ENABLE_NOTIFY=ON` (on), so example / test builds
  get notification without extra flags.
- `include/rb_notify.h` is a compatibility shim that simply
  `#include "rb.h"`. Prefer including `rb.h` directly.

Notification is a **wake-up hint**, not part of the ownership protocol.
Missed or spurious wakes are always safe as long as the waiter fully
drains (or publishes) after returning from the wait.

### Snapshot-before-drain (required pattern)

The correct consumer loop snapshots the producer sequence **before**
draining. Draining first and then snapshotting can lose a publish that
arrives between the two calls:

```c
for (;;) {
    uint32_t v = rb_notify_value(rb);   /* snapshot FIRST */
    if (rb_drain(rb, on_item, user) > 0)
        continue;                       /* made progress */
    if (done)
        break;
    int rc = rb_wait(rb, v, 100);       /* block until sequence != v */
    /* rc == 0  → woke (or data already present)
       rc < 0   → -ETIMEDOUT / -EINTR / other errno */
}
```

### `rb_notify_value`

```c
uint32_t rb_notify_value(const rb_t *rb);
```

Returns the current producer sequence word (`head` in Mode 0,
`notify_seq` in Mode 1). Pass this value as the `expected` argument of
`rb_wait`. Returns `0` if `rb` is `NULL`.

### `rb_wait`

```c
int rb_wait(rb_t *rb, uint32_t expected, int timeout_ms);
```

Block the calling thread while the ring is empty **and** the producer
sequence still equals `expected`. Uses a futex on the same word that
`rb_publish` advances, so publication and notification share one
monotonically increasing `uint32_t`.

| `timeout_ms` | Behaviour |
|---|---|
| `< 0` | Block indefinitely (internally sliced by `RB_NOTIFY_WAIT_SLICE_MS` to avoid ABA on 32-bit wrap) |
| `0` | Poll once; return immediately |
| `> 0` | Bound the wait to that many milliseconds |

**Returns:**

- `0` — data is available, or a wake occurred (caller must drain)
- `-ETIMEDOUT` — timeout expired with the ring still empty
- `-EINVAL` — `rb` is `NULL` or not initialised
- other negative errno values on unexpected futex failure

If the ring is already non-empty when `rb_wait` is entered, it returns
`0` without sleeping.

Cross-process note: the default futex is `FUTEX_WAIT_PRIVATE`
(`RB_FUTEX_SHARED=0`). That works between threads of one process, not
across processes sharing `MAP_SHARED` memory. For cross-process
wake-ups use `rb_notify_fd()` (eventfd) or set `RB_FUTEX_SHARED=1`.

### `rb_notify_fd` / `rb_notify_drain_fd`

```c
int rb_notify_fd(rb_t *rb);
int rb_notify_drain_fd(rb_t *rb);
```

`rb_notify_fd` returns a non-blocking `eventfd` suitable for
`poll` / `epoll` / libuv / libev. Create it once (before the producer
starts); subsequent calls return the same descriptor. Returns a
negative errno on failure.

After the fd becomes readable, call `rb_notify_drain_fd` to clear the
counter, then drain the ring. Returns `0` on success, negative errno
on failure.

**Example — event-loop integration:**

```c
int fd = rb_notify_fd(rb);
/* register fd with epoll / uv_poll / … */

/* in the readable callback: */
rb_notify_drain_fd(rb);
rb_drain(rb, on_item, user);
```

### Related compile-time options

| Macro | Default | Effect |
|---|---|---|
| `RB_ENABLE_NOTIFY` | `0` (header) / `ON` (CMake) | Compile the functions above |
| `RB_FUTEX_SHARED` | `0` | Use process-shared futex ops |
| `RB_NOTIFY_WAIT_SLICE_MS` | `100` | Max individual futex sleep (ms) |

---

## Error codes

| Code | Meaning |
|---|---|
| `RB_OK` | Success |
| `RB_ERR_FULL` | Ring at `limit`; producer must wait |
| `RB_ERR_EMPTY` | Ring empty; consumer must wait |
| `RB_ERR_OVERSIZE` | Payload > slot capacity and policy is `DROP` |
| `RB_ERR_INVAL` | Bad argument, mis-pairing, or failed validation |
| `RB_ERR_NOT_INIT` | `rb` is NULL or scratchpad not installed |

`rb_err_t` values are stable: they are used in tests, and reordering the
enum would break `RB_ERR_*` numeric comparisons in user code that
persists them.

---

## Complete worked example

A small, self-contained program that sets up the ring, spawns a
producer and consumer, and runs for a bounded number of items.

```c
#define _POSIX_C_SOURCE 200809L

#include "rb.h"
#include "rb_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#define CAPACITY   64u
#define SLOTS      64u
#define SLOT_SIZE  2048u
#define ITEMS      100000u

typedef struct {
    rb_t *rb;
    atomic_uint_fast64_t produced;
    atomic_uint_fast64_t consumed;
    atomic_bool done;
} ctx_t;

static void *xaligned(size_t align, size_t sz) {
    void *p = NULL;
    if (posix_memalign(&p, align, sz) != 0 || !p) abort();
    return p;
}

static void *producer(void *arg) {
    ctx_t *c = arg;
    for (uint32_t i = 0; i < ITEMS; i++) {
        uint32_t idx, cap;
        void *w;
        while (rb_acquire(c->rb, 32, &idx, &w, &cap) == RB_ERR_FULL)
            usleep(10);
        int n = snprintf(w, 32, "msg %u", i);
        rb_publish(c->rb, idx, (uint32_t)n);
        atomic_fetch_add(&c->produced, 1);
    }
    atomic_store(&c->done, true);
    return NULL;
}

static bool on_item(const void *obj, uint32_t len, bool trunc, void *user) {
    ctx_t *c = user;
    (void)obj; (void)len; (void)trunc;
    atomic_fetch_add(&c->consumed, 1);
    return true;
}

static void *consumer(void *arg) {
    ctx_t *c = arg;
    for (;;) {
        if (rb_drain(c->rb, on_item, c) == 0 && atomic_load(&c->done))
            break;
        usleep(10);
    }
    return NULL;
}

int main(void) {
    rb_t *rb = xaligned(RB_CACHE_LINE, rb_size(CAPACITY));
    void *scratch = xaligned(RB_CACHE_LINE, SLOTS * SLOT_SIZE);

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = CAPACITY;
    cfg.slots     = SLOTS;
    cfg.slot_size = SLOT_SIZE;

    if (rb_init(rb, &cfg, scratch, SLOTS * SLOT_SIZE) != RB_OK) {
        fprintf(stderr, "rb_init failed\n");
        return 1;
    }

    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rb = rb;
    atomic_init(&ctx.produced, 0);
    atomic_init(&ctx.consumed, 0);
    atomic_init(&ctx.done, false);

    rb_thread_t tp, tc;
    rb_thread_spawn(rb, &tp, true,  producer, &ctx);
    rb_thread_spawn(rb, &tc, false, consumer, &ctx);
    rb_thread_join(&tp);
    rb_thread_join(&tc);

    const rb_stats_t *st = rb_stats(rb);
    printf("produced=%llu consumed=%llu\n",
           (unsigned long long)atomic_load(&ctx.produced),
           (unsigned long long)atomic_load(&ctx.consumed));
    if (st)
        printf("stats: high_water=%u full_hits=%u low_d_hits=%u\n",
               st->high_water, st->full_hits, st->low_d_hits);

    rb_deinit(rb);
    free(scratch);
    free(rb);
    return 0;
}
```

Compile it against the library:

```bash
cmake -B build -DRB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build
cc -std=c11 -O2 -Iinclude example.c -Lbuild -lrb -lpthread -o example
./example
```

Expected output (numbers approximate):

```
produced=100000 consumed=100000
stats: high_water=64 full_hits=NN low_d_hits=MM
```

---

## See also

- [Technical notes](TECHNICAL.md) — design rationale, config matrix,
  benchmarks, porting.
- [Overview](../README.md) — what it is and why.
