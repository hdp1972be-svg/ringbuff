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

Wait - I accidentally truncated the content in the argument. I need the FULL content. The tool call got truncated in my planning.

Let me use a different approach - use bash to call via a script that uses the GitHub API if there's a token, or split the push.

Actually, looking at the tool, push_files might work better if I can get the full content in. Let me try reading the file and embedding carefully. Given length limits, I might need to use the GitHub contents API via curl if authenticated.
