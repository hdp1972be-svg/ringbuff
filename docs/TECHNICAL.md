# rb — Technical design

This document describes how `rb` is implemented internally, covering both
execution modes and the reasoning behind the hot-path layout. It complements
`docs/API.md` (public interface), `docs/CACHE_LAYOUT.md` (alignment guidance)
and `docs/README.md` (model and examples).

## Two execution modes

The library is compiled in one of two modes, selected by a single compile-time
flag:

| Flag                     | Mode 0 (default)        | Mode 1          |
| ------------------------ | ------------------------ | --------------- |
| `RB_PER_SLOT_LAP`        | `0`                      | `1`             |
| Ring primitive           | `head`/`tail` + `entries[]` | per-slot lap counters + `notify_seq`/`consumer_pos` |
| Slot header size         | 4 bytes                  | 8 bytes (seq + hdr) |
| Physical FIFO depth      | `min(capacity, slots)`  | `slots`         |

The two modes share the full public API. Mode 0 is the historical SPSC ring
of *references* into the scratchpad; Mode 1 implements the Vyukov per-slot
sequence-stamping scheme and drops the `entries[]` indirection entirely.

The `rb_s` control block and the slot layout are selected with
`#if RB_PER_SLOT_LAP / #else / #endif` blocks so the *default* build (Mode 0)
keeps its historical layout bit-for-bit. The two modes never mix at runtime:
the flag is baked in at compile time, and a binary is built against exactly one
mode.

## Shared concepts

### Positions are monotonic, indices wrap

In both modes the queue is driven by a pair of ever-increasing **positions**
(counts), not by wrapping indices:

- producer position = number of messages ever acquired/published,
- consumer position = number of messages ever consumed/released.

The physical slot used for a given position is `position mod slots`
(`slot_index_for()`). Positions are `uint32_t`; a full 32-bit lap is far beyond
the lifetime of any real queue, so wrap-around arithmetic (`pos - tail`) is
safe and intentional.

### Scratchpad

A caller-owned block of `slots` slots, each `slot_size` bytes, stride-aligned
(`slot_stride_for()` rounds up to `RB_CACHE_LINE` when `RB_SLOT_CACHELINE_PAD`
is on, else `_Alignof(max_align_t)`, minimum 4). The scratchpad base must be
aligned to the same bound (`rb_init` rejects a misaligned scratch). Slots are
referenced by `index * rb->slot_stride`, never `index * slot_size`.

`rb_init` requires `slot_size > RB_SLOT_HDR_SIZE`: every slot must hold at
least its header. Payload capacity of a slot is `slot_size - RB_SLOT_HDR_SIZE`.

### Slot header

Immediately before the payload, every slot carries a 4-byte header:

```
 bit 31         : truncated flag   (RB_SLOT_TRUNCATED = 0x80000000u)
 bits 30..0     : payload length    (RB_SLOT_LEN_MASK   = 0x7FFFFFFFu)
```

Because the header is read and written with `memcpy` at
`slot + (RB_SLOT_HDR_SIZE - sizeof hdr)`, it is at offset 0 in Mode 0 and
offset 4 in Mode 1 — the idiom is shared between modes and folds to the same
code when `RB_SLOT_HDR_SIZE` is 4.

### Oversize policy

`cap = slot_size - RB_SLOT_HDR_SIZE`. If a caller asks for more than `cap`:

- `RB_OVERSIZE_TRUNCATE` (default): payload is capped to `cap`, `truncated` is
  forced true, the message is published normally, the slot reads back shorter,
- `RB_OVERSIZE_DROP`: `rb_acquire` returns `RB_ERR_OVERSIZE` and no slot is
  taken.

## Mode 0 — ring of references

### Control block

Producer-owned: `head` (release-store, cache-line aligned), `cached_tail`.
Consumer-owned: `tail` (release-store, cache-line aligned), `cached_head`.
Shared/other: `full_latch`, `pending_slot`/`pending_wanted`, notify state,
`consumer_active`/`consumer_slot`, low-water latches, geometry, callbacks,
stats.

`entries[]` is a circular array of `capacity` entries, each a `uint32_t` slot
index (or a slot pointer with `RB_USE_POINTERS=1`). `capacity` must be a power
of two; entry index is `pos & rb->mask`.

### Hot paths

- **acquire**: `head = load_rlx(head)`, `count = head - cached_tail`. If
  `count >= limit`, refresh the consumer tail once (acquire load) and recheck.
  On failure return `RB_ERR_FULL`. Otherwise the free entry is at
  `entries[head & mask]`; record `pending_slot`/`pending_wanted` and hand the
  caller a pointer to `slot + 4` and `cap`.
- **publish**: write the header, store the slot ref into
  `entries[head & mask]`, release-store `head + 1` (publish). If waiters or an
  eventfd exist, wake them. Refresh `cached_tail`, compute the new count, and
  run `on_slot_added` / `on_full`.
- **consume**: `tail = load_rlx(tail)`, `head = cached_head`; if equal, refresh
  `head` (acquire) and return `RB_ERR_EMPTY` when still equal. Read the entry
  from `entries[tail & mask]`, derive the slot index, read the header at
  `slot + 0`, and latch `consumer_active`.
- **release**: release-store `tail + 1`. The slot becomes eligible for reuse as
  soon as `tail` catches up; the entry in the ring has already been cleared by
  the producer overwriting it.

### Invariants

- Producer may not reuse ring entry `k` until consumer has advanced past `k`
  (guaranteed because the producer only writes entry `head`, and `head - tail`
  is bounded by `limit ≤ capacity`).
- Physical scratch slots are reused only when safe: the same scratch slot must
  not back two live ring entries, which requires the in-flight depth to stay
  ≤ `slots`. `rb_init` therefore clamps `limit = min(limit, capacity, slots)`,
  so the queue can never hold more live messages than physical slots.
- `count = head - tail`; `rb_count()` acquires both. `rb_is_full`/`rb_is_empty`
  derive from it.

## Mode 1 — per-slot lap sequence

### Control block

The `entries[]` ring is gone from the hot path. The position counters take the
place of the Mode-0 fields (same field offsets where it matters):

| Mode 1 field              | Mode 0 field       | Owner      |
| ------------------------- | ------------------ | ---------- |
| `notify_seq`              | `head`             | producer   |
| `cached_consumer_pos`     | `cached_tail`      | producer   |
| `consumer_pos`            | `tail`             | consumer   |
| `cached_notify_seq`       | `cached_head`      | consumer   |

`notify_seq` and `consumer_pos` are release/acquire atomics, each on its own
cache line (matching Mode 0's `head`/`tail` placement). `entries[]` still
exists in the struct so `rb_size(capacity)` and the Mode-0 layout are
unchanged, but it is unused in Mode 1.

### Slot layout

Each scratch slot now begins with a 4-byte **sequence stamp** followed by the
message header:

```
 offset 0  : sequence stamp  (rb_atomic_u32)
 offset 4  : message header  (len | truncated)
 offset 8  : payload         (slot + RB_SLOT_HDR_SIZE, RB_SLOT_HDR_SIZE = 8)
```

The sequence stamp is a per-slot generation that encodes which position the
slot currently holds. It is stamped at publish time, checked at consume time,
and re-stamped at release time so a slot is never reused while its data might
still be read.

### Initialization

`rb_init` does a deterministic preseed loop over the scratch:
`slot_seq[si] = si` for every slot, so all generations are defined before the
first publish. Both position counters start at 0.

### Hot paths

- **acquire**: `pos = load_rlx(notify_seq)`, `count = pos - cached_consumer_pos`.
  If `count >= limit`, refresh `consumer_pos` (acquire) and recheck; full →
  `RB_ERR_FULL`. Because `limit ≤ slots`, the target slot for position `pos`
  has been fully released by the consumer and is free — no stamp check is
  needed on the producer side. Record `pending_slot`/`pending_wanted`; the
  caller writes payload at `slot + 8`.
- **publish**: write the header at `slot + 4`, then
  `release-store(slot_seq, pos + 1)` and `release-store(notify_seq, pos + 1)`.
  Stamping the slot first and the global counter second is what makes the
  payload, header and stamp visible in the right order to the consumer. If
  waiters or an eventfd exist, wake them; refresh `cached_consumer_pos`,
  compute `count = (pos + 1) - tail`, run callbacks / `on_full`.
- **consume**: `tail = load_rlx(consumer_pos)`, `head = cached_notify_seq`; if
  equal, refresh `head` (acquire) and return `RB_ERR_EMPTY` when still equal.
  The message for position `tail` lives in `slot_index_for(rb, tail)`. Probe the
  stamp: `acquire-load(slot_seq)`. Matching `tail + 1` confirms this slot holds
  position `tail` and its data is fully published. A non-matching stamp means
  the producer has not reached `tail` yet — refresh `cached_notify_seq`; if
  `head == tail` the queue is empty, otherwise retry the same slot against the
  refreshed position. On success read the header at `slot + 4` and latch
  `consumer_active`.
- **release**: `release-store(slot_seq, tail + slots)` FIRST, then
  `release-store(consumer_pos, tail + 1)`. The recycle stamp plus the
  `consumer_pos` release store publishes both the free slot and the advance to
  the producer. `consumer_active = 0`.

The slot-stamp check is a *recent-winner probe*: the common case (the consumer
is only one publish behind) reads the stamp without touching `notify_seq`, so
the consumer's steady-state path touches only `consumer_pos`, the slot stamp,
and the slot itself.

### Why `limit ≤ slots` guarantees single-ownership

The producer writing position `pos` targets
`slot_index_for(rb, pos)`. That slot was last held by position `pos - slots`.
For the producer to race against a live message, the consumer must still be
holding position `pos - slots`, i.e. `pos - consumer_pos > slots`. The acquire
path enforces `pos - consumer_pos < limit ≤ slots`, so the target slot has been
released (`consumer_pos > pos - slots`) before the producer touches it. No
ABA problem exists on the stamp: generation values strictly increase per slot,
and position `p`'s stamp (`p + 1`) differs from both its predecessor's
(`p`, or the recycle stamp `p - slots`) and its successor's.

### Memory ordering

- **Release-acquire pair on the stamp.** `release-store(slot_seq, pos+1)`
  publishes every earlier plain store (payload + header) to the consumer that
  `acquire-load`s that stamp and sees `tail + 1`. The slot-stamp probe is the
  "peek" that lets a consumer decide data is ready cheaply.
- **Release-acquire pair on the counters.** `release-store(notify_seq, pos+1)`
  is the authoritative hand-off; `acquire-load(notify_seq)` in consume, `count`
  and `rb_notify_value` synchronize against it.
- **Release-acquire pair on release.** `release-store(consumer_pos, tail+1)`
  publishes the recycle stamp and frees the slot to the producer, whose
  `acquire-load(consumer_pos)` in the acquire refresh path sees it.

Result: a producer and consumer never touch the same cache line with plain
stores, and every visibility requirement is met through one of the three
release-acquire pairs above.

## Cross-cutting behaviour

### Notify

When `RB_ENABLE_NOTIFY` and Linux are active:

- Publish wakes a futex on the producer counter (`head` in Mode 0, `notify_seq`
  in Mode 1, both cache-line-aligned so the address is futex-safe) when
  `notify_waiters != 0`, and writes a byte to `notify_fd` if one exists.
- `rb_wait(expected, timeout_ms)` blocks on the same futex word, re-checking
  `rb_count()` each slice; `rb_notify_value()` returns the current producer
  counter (`head` in Mode 0, `notify_seq` in Mode 1), which is exactly the
  value the waiter should pass as `expected`.
- `rb_count()` = `notify_seq - consumer_pos` in Mode 1 (acquire loads both),
  `head - tail` in Mode 0 — so a spurious wake on either counter is safe.

### Watchers of the counter difference

Several helpers depend only on `rb_count()` (`rb_is_full`, `rb_is_empty`,
`rb_set_limit`'s emptiness check), so they are mode-agnostic.
`rb_notify_value()` and the futex word are the only notify surfaces that need
the mode branch, and both resolve to the producer counter.

### Stats and callbacks

`published`, `consumed`, `high_water`, `truncated`, `full_attempts`,
`full_hits`, `low_d_hits`, `low_e_hits` are tracked identically in both modes.
`on_slot_added`, `on_full`, `on_low_d`, `on_low_e` fire at the same points.
Neither mode's bookkeeping sees the other's.

### Geometry and capacity semantics

|                  | Mode 0                       | Mode 1                       |
| ---------------- | ---------------------------- | ---------------------------- |
| Ring entries     | `capacity` (`entries[]`)     | none (scratch only)          |
| Max in-flight    | `limit ≤ min(capacity, slots)` | `limit ≤ slots`            |
| `rb_capacity()`  | configured `capacity`        | configured `capacity`        |
| `rb_limit()`     | `limit`                      | `limit`                      |
| scratch needed   | `slots * stride`             | `slots * stride`             |
| `rb_size()`      | `offsetof + capacity * sizeof(rb_entry_t)` (unchanged, keeps layout stable) | same |

`rb_size(capacity)` is identical in both modes — the ring entry array remains
in the struct even though Mode 1 does not use it — so binaries and IPC layouts
allocated with `rb_size()` do not change when switching modes. The practical
difference is throughput and cache behaviour, not the API or the memory
footprint of the control block.

## Choosing a mode

- **Mode 0** keeps the exact historical layout and semantics; choose it for
  drop-in compatibility or when the indirection cost is irrelevant.
- **Mode 1** removes the `entries[]` round-trip on both the producer and
  consumer hot paths and trades the ring array for a single cache-line
  stamp probe per slot. It is the faster mode for small fixed-size messages
  at high message rates.

Both modes produce identical observable behaviour through the public API
(FIFO order, zero-copy, backpressure, callbacks, notify), and the full test
suite (`rb_all`, `rb_per_slot`) validates Mode 0 and Mode 1 respectively.

## Building and testing

Mode 0 is the default and is tested by the `rb_all` target. Mode 1 is a
separate static library (`rb_per_slot`), built from the same `src/rb.c` with
`RB_PER_SLOT_LAP=1` and exercised by the `test_rb` suite under the
`rb_per_slot` ctest name. Both share the identical behavioral tests, so a
feature is only considered done when it passes in both modes:

```
cmake -S . -B build -DRB_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```