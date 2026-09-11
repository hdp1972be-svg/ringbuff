API at a glance
Function	Purpose
rb_size(capacity)	Byte size of the control block
rb_init(rb, cfg, scratch, size)	Install a caller-owned scratchpad
rb_acquire(rb, wanted, &idx, &w, &cap)	Get a writable slot
rb_publish(rb, idx, len)	Publish a filled slot
rb_abort(rb)	Abort an in-flight acquire
rb_consume(rb, &idx, &obj, &len, &trunc)	Get a readable slot
rb_release(rb, idx)	Return a slot to the producer
rb_drain(rb, fn, user)	Consume every available slot
rb_flush(rb, fn, user)	Publish until full or fn says stop
rb_set_limit / rb_set_low_d / rb_set_low_e	Runtime policy
rb_set_callbacks	Replace the four informational callbacks
rb_set_oversize_policy	TRUNCATE (default) or DROP
rb_stats(rb)	Watermarks and counters (if compiled in)

Callbacks:
Callback	Fires in	Trigger	Latched
on_slot_added	producer	every publish	no
on_full	producer	count reaches limit	yes
on_low_d	consumer	count crosses below d%	yes
on_low_e	consumer	count crosses below e%	yes

---
FULL API


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
#include "rb.h"          // always
#include "rb_thread.h"   // if RB_ENABLE_THREAD_HELPERS
#include "rb_notify.h"   // if RB_ENABLE_NOTIFY
```

`rb.h` transitively includes `rb_config.h`. It has `extern "C"` guards,
so it is safe to include from C++.

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
#define RB_SLOT_HDR_SIZE  4u
```

Every slot begins with a 4-byte header. Bits 0–30 hold the payload
length; bit 31 is the `TRUNCATED` flag. Effective payload capacity per
slot is `slot_size - RB_SLOT_HDR_SIZE`.

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
| `RB_ENABLE_STATS` | `1` | 1 = compile in counters |
| `RB_ENABLE_THREAD_HELPERS` | `1` | 1 = build `rb_thread.c` |
| `RB_ENABLE_NOTIFY` | `0` | 1 = build `rb_notify.c` |
| `RB_DEFAULT_LOW_D` | `25` | Default low_d percentage |
| `RB_DEFAULT_LOW_E` | `10` | Default low_e percentage |

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

Replace the callback table and user pointer. Safe to call at any time,
but the caller must ensure no callback is currently executing on another
thread.

### `rb_set_producer_stack` / `rb_set_consumer_stack`

```c
rb_err_t rb_set_producer_stack(rb_t *rb, size_t bytes);
rb_err_t rb_set_consumer_stack(rb_t *rb, size_t bytes);
```

Change the stack size used by the next `rb_thread_spawn`. Has no effect
on already-spawned threads. Passing `0` restores the compile-time
default.

---

## Stats

### `rb_stats`

```c
#if RB_ENABLE_STATS
const rb_stats_t *rb_stats(const rb_t *rb);
#endif
```

Returns a pointer to the ring's stats structure. Safe to read from
either side, but the counters are not atomic — read-only inspection from
a single thread is fine.

**Fields:**

- `published` — successful `rb_publish` calls
- `consumed` — successful `rb_release` calls
- `truncated` — publishes where the `TRUNCATED` flag was set
- `full_attempts` — `rb_acquire` calls that returned `RB_ERR_FULL`
- `high_water` — max `rb_count` ever observed
- `full_hits` — number of times `on_full` fired (latched episodes)
- `low_d_hits` — number of times `on_low_d` fired
- `low_e_hits` — number of times `on_low_e` fired

**Example:**

```c
const rb_stats_t *st = rb_stats(rb);
printf("published=%llu consumed=%llu high_water=%u full_hits=%u\n",
       (unsigned long long)st->published,
       (unsigned long long)st->consumed,
       st->high_water, st->full_hits);
```

### `rb_stats_reset`

```c
#if RB_ENABLE_STATS
void rb_stats_reset(rb_t *rb);
#endif
```

Zero all counters. Caller must ensure no other thread is publishing or
consuming when this is called.

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
- `RB_ERR_INVAL` — bad args, `pthread_attr_init` failed, or `pthread_create` failed

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

Available when `RB_ENABLE_NOTIFY=1`. Off by default. Included via
`#include "rb_notify.h"`.

Two independent wake-up channels:

- **data** — producer signals, consumer waits ("new item published")
- **space** — consumer signals, producer waits ("slot freed")

Backends selected by `RB_NOTIFY_BACKEND`:

- `RB_NOTIFY_EVENTFD` — Linux and Android (default where available)
- `RB_NOTIFY_PIPE` — reserved, **not currently implemented**
- `RB_NOTIFY_NONE` — no-OS; all operations are cheap no-ops

### Types

```c
typedef struct {
    int data_fd;
    int space_fd;
} rb_notify_t;
```

### `rb_notify_init` / `rb_notify_destroy`

```c
rb_err_t rb_notify_init(rb_notify_t *n);
void     rb_notify_destroy(rb_notify_t *n);
```

Initialise / release the underlying fds. On the `NONE` backend, `init`
always returns `RB_OK` and both fds are `-1`.

### `rb_notify_signal_data` / `rb_notify_signal_space`

```c
void rb_notify_signal_data (rb_notify_t *n);
void rb_notify_signal_space(rb_notify_t *n);
```

Signal the other side. Cheap and idempotent. Missed signals are safe as
long as the waiter fully drains the ring after each wake-up.

### `rb_notify_wait_data` / `rb_notify_wait_space`

```c
rb_err_t rb_notify_wait_data (rb_notify_t *n, int timeout_ms);
rb_err_t rb_notify_wait_space(rb_notify_t *n, int timeout_ms);
```

Block until the corresponding channel signals. `timeout_ms`: `<0` blocks
forever, `0` polls once, `>0` bounded wait.

**Returns:** `RB_OK` on wakeup, `RB_ERR_EMPTY` on timeout.

### Raw fds

```c
int rb_notify_data_fd (const rb_notify_t *n);
int rb_notify_space_fd(const rb_notify_t *n);
```

For integrating into an `epoll`/`poll`/`select` loop.

**Example — producer signals, consumer waits:**

```c
/* Producer: after a successful rb_publish */
rb_notify_signal_data(&notify);

/* Consumer loop */
for (;;) {
    rb_notify_wait_data(&notify, -1);
    rb_drain(rb, process_frame, NULL);
}
```

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
    rb_t  *rb;
    atomic_uint_fast64_t produced;
    atomic_uint_fast64_t consumed;
    atomic_bool done;
} ctx_t;

static void *xaligned(size_t align, size_t n) {
    void *p = NULL;
    if (posix_memalign(&p, align, n) != 0 || !p) abort();
    return p;
}

/* ---------------- callbacks ---------------- */

static void on_slot_added(rb_t *rb, uint32_t idx, uint32_t len,
                          bool trunc, void *user) {
    (void)rb; (void)idx; (void)user;
    if (trunc) fprintf(stderr, "truncated publish (len=%u)\n", len);
}

static void on_full(rb_t *rb, void *user) {
    (void)rb; (void)user;
    /* producer would set stop_reading = true here */
}

static void on_low_d(rb_t *rb, void *user) {
    (void)rb; (void)user;
    /* producer would set stop_reading = false here */
}

/* ---------------- producer ---------------- */

static void *producer(void *arg) {
    ctx_t *c = arg;
    for (uint64_t seq = 0; seq < ITEMS; ) {
        uint32_t idx, cap;
        void *w;
        rb_err_t e = rb_acquire(c->rb, sizeof seq, &idx, &w, &cap);
        if (e == RB_ERR_FULL) { usleep(10); continue; }
        if (e != RB_OK) break;

        memcpy(w, &seq, sizeof seq);
        rb_publish(c->rb, idx, sizeof seq);
        atomic_fetch_add(&c->produced, 1);
        seq++;
    }
    atomic_store(&c->done, true);
    return NULL;
}

/* ---------------- consumer ---------------- */

static uint64_t last_seen = 0;
static uint64_t gaps = 0;

static bool process(const void *obj, uint32_t len,
                    bool trunc, void *user) {
    (void)user;
    if (trunc || len != sizeof(uint64_t)) return true;
    uint64_t v;
    memcpy(&v, obj, sizeof v);
    if (v != last_seen + 1 && last_seen != 0) gaps++;
    last_seen = v;
    return true;
}

static void *consumer(void *arg) {
    ctx_t *c = arg;
    while (last_seen + 1 < ITEMS) {
        uint32_t n = rb_drain(c->rb, process, NULL);
        atomic_fetch_add(&c->consumed, n);
        if (n == 0) {
            if (atomic_load(&c->done) && rb_is_empty(c->rb)) break;
            usleep(10);
        }
    }
    return NULL;
}

/* ---------------- main ---------------- */

int main(void) {
    rb_t *rb = xaligned(RB_CACHE_LINE, rb_size(CAPACITY));
    void *scratch = xaligned(RB_CACHE_LINE, SLOTS * SLOT_SIZE);

    rb_callbacks_t cb = {
        .on_slot_added = on_slot_added,
        .on_full       = on_full,
        .on_low_d      = on_low_d,
    };

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = CAPACITY;
    cfg.slots     = SLOTS;
    cfg.slot_size = SLOT_SIZE;
    cfg.low_d     = 25;
    cfg.low_e     = 10;
    cfg.cb        = cb;

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
    printf("produced=%llu consumed=%llu gaps=%llu\n",
           (unsigned long long)atomic_load(&ctx.produced),
           (unsigned long long)atomic_load(&ctx.consumed),
           (unsigned long long)gaps);
    printf("stats: high_water=%u full_hits=%u low_d_hits=%u\n",
           st->high_water, st->full_hits, st->low_d_hits);

    rb_deinit(rb);
    free(scratch);
    free(rb);
    return gaps == 0 ? 0 : 1;
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
produced=100000 consumed=100000 gaps=0
stats: high_water=64 full_hits=NN low_d_hits=MM
```

---

## See also

- [Technical notes](TECHNICAL.md) — design rationale, config matrix,
  benchmarks, porting.
- [Overview](../README.md) — what it is and why.


