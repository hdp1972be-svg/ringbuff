# Direct slot mapping experiment

## Purpose

`RB_DIRECT_SLOT_MAP=1` removes the `entries[]` metadata lookup when the ring has exactly one scratch slot per logical ring position (`slots == capacity`).

The normal ring cursor semantics are deliberately unchanged:

- `head` and `tail` remain logical sequence numbers.
- One publish/release advances a cursor by `1`.
- Notification and futex sequence values therefore retain their normal meaning.
- Slot selection is derived from the cursor with the ring mask.
- The scratch address is then `scratch + slot_index * slot_stride`.

This experiment is intentionally narrower than `RB_CURSOR_ENCODES_SCRATCH`. It tests only whether the `entries[]` indirection is worth eliminating.

## Constraints

The option rejects:

- `RB_CURSOR_ENCODES_SCRATCH`
- `RB_USE_POINTERS`
- `RB_PER_SLOT_LAP`
- configurations where `slots != capacity`

The restriction to `slots == capacity` is important: with one scratch slot per logical ring position, the slot index is simply `head & (capacity - 1)` or `tail & (capacity - 1)`. Configurations with fewer or more scratch slots still require the existing `entries[]` mapping.

## Hot path

The default path is conceptually:

    head -> entries[head & mask] -> slot_index -> scratch + slot_index * stride

The experiment changes this to:

    head -> head & mask -> scratch + slot_index * stride

The same mapping is used for `tail` on consume. Publishing no longer writes the corresponding `entries[]` element.

## Why this experiment is preferred over encoded cursors

`RB_CURSOR_ENCODES_SCRATCH` changes the representation and arithmetic of the public ring state: the cursor becomes a byte offset rather than a logical sequence number. That also changes notification values and introduces shift/mask arithmetic on cursor transitions.

`RB_DIRECT_SLOT_MAP` changes only the internal slot lookup. It therefore gives a cleaner A/B measurement of the cost of the metadata indirection without changing ring semantics.

## Benchmarking

The benchmark reports **iterations/s** and **ns/iteration**. One iteration is:

    rb_acquire -> rb_publish -> rb_consume -> rb_release

The benchmark currently measures the metadata/control path; it does not deliberately consume the payload. A separate payload benchmark should be used when evaluating cache locality or memcpy/data-path effects.

For meaningful A/B results, use identical compiler flags and run the default and experiment builds under comparable CPU-frequency and thermal conditions. Repeated `perf stat` measurements are preferable to a single wall-clock sample.
