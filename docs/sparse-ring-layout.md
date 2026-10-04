# Sparse cache-line ring entry layout

## Purpose

The experimental `exp/sparse-ring-entries` branch changes only the default
`entries[]` indirection used when `RB_PER_SLOT_LAP=0`.

The legacy ring stores entries densely:

```text
entries + 0x00   slot 0
entries + 0x04   slot 1
entries + 0x08   slot 2
...
```

The experimental layout reserves one cache-line-sized stride for every
logical ring entry:

```text
entries + 0x000   slot 0
entries + 0x040   slot 1
entries + 0x080   slot 2
entries + 0x0c0   slot 3
...
```

With the default 64-byte cache line and capacity 64, the physical ring
occupies 4096 bytes. The flex-array base is explicitly cache-line aligned,
so the first entry and every subsequent entry start on a cache-line boundary.

## Cursor representation

The default `head` and `tail` remain monotonically increasing sequence
counters. Their unit changes from logical entries to bytes:

```text
legacy:       0, 1, 2, 3, ...
experimental: 0, 64, 128, 192, ...
```

They are **not** wrapped at the physical ring size. The physical address uses
the low ring bits:

```text
physical_offset = cursor & ring_mask
```

This preserves the existing unsigned sequence-counter occupancy semantics
while allowing the address calculation to use the cursor directly.

Occupancy is therefore:

```text
(head - tail) / RB_RING_ENTRY_STRIDE
```

The slot index for the scratchpad is derived independently:

```text
slot_index = (cursor / RB_RING_ENTRY_STRIDE) mod slots
```

The scratchpad layout itself is unchanged.

## Configuration

`RB_RING_ENTRY_STRIDE` controls the layout:

- default experimental value: `RB_CACHE_LINE`
- set it to `0` to restore the legacy dense `entries[]` layout

A non-zero stride must be a power of two, must be at least the cache-line
size, and must fit the selected entry type.

## Scope

This experiment deliberately does **not** change:

- scratchpad slot placement;
- slot stride;
- payload alignment;
- acquire/publish/consume/release API;
- the per-slot-lap implementation;
- atomics or ownership ordering;
- hardware cache-maintenance hooks.

The objective is to measure only the cost of changing the ring-entry address
representation.

## Expected trade-off

For capacity 64 and a 64-byte stride, the ring consumes 4 KiB instead of the
legacy 256 bytes, but the ring metadata can remain entirely inside a typical
32 KiB L1 data cache.

For capacity 1024 the sparse ring consumes 64 KiB, so L1 residency is no
longer possible on processors with a 32 KiB L1D. This configuration therefore
has to be benchmarked independently rather than assuming that the sparse
layout is universally faster.

## Benchmarking

Build the normal benchmark and compare two builds:

```sh
cc -O2 -std=c11 -Iinclude bench/bench_rb.c src/rb.c -o /tmp/bench_rb_sparse
```

For a dense control build:

```sh
cc -O2 -std=c11 -DRB_RING_ENTRY_STRIDE=0 -Iinclude \
   bench/bench_rb.c src/rb.c -o /tmp/bench_rb_dense
```

Compare both with the same CPU affinity and system conditions. For cycle-level
analysis, inspect the generated assembly and use `perf stat` separately for
each binary. In particular compare cycles, instructions, L1D misses and LLC
activity rather than relying only on wall-clock nanoseconds.

This branch is an experiment; the sparse layout should not be merged into the
default branch until correctness tests and benchmark results demonstrate a
repeatable benefit.
