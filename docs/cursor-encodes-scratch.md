# Encoded scratchpad cursor experiment

## Purpose

This branch tests a different cursor representation for the default SPSC implementation.
Instead of treating head and tail as logical ring-entry counters and looking up the corresponding scratch slot, the cursor value itself is the byte offset of the current scratchpad slot.
For a ring whose scratchpad is slots × slot_stride bytes, the cursor sequence is 0, stride, 2*stride, ..., (slots-1)*stride, 0, ... .
Thus the consumer can obtain the slot address directly from tail: scratch + tail, and publication does not write an entries[] mapping.

## Constraints

This is deliberately an isolated performance experiment.
- slots == capacity is required.
- slot_stride must be a power of two.
- The complete scratch ring must be at most 2 GiB.
- RB_USE_POINTERS is not supported by this representation.
- RB_PER_SLOT_LAP is not changed by this experiment.
- entries[] remains in the control-block layout for ABI isolation, but is not accessed by the default publish/consume path.
- notification values based on head now represent scratchpad byte offsets, so this mode is not ABI/semantic compatible with code that interprets rb_notify_value() as a logical publication count.

## Why this differs from direct slot mapping

The earlier direct-slot experiment retained logical sequence numbers in head/tail and computed slot_index = head & (slots - 1).
This experiment changes the representation itself. The cursor is already a scratchpad position, so the consume path can address scratch + tail without an entries[] load or a slot-index-to-byte-offset multiply.
Because the stride is required to be a power of two, slot-index extraction and occupancy conversion can be compiled as shifts.

## Expected trade-off

The hot path removes the metadata mapping, but it introduces cursor wrap arithmetic, conversion between byte distance and slot count for occupancy, and a different meaning for the observable head/tail sequence.
The benchmark should therefore decide this experiment empirically. It should be compared against the dense baseline with identical compiler flags and runtime configuration.

## Build

```sh
cmake -S . -B build-cursor -DRB_BUILD_BENCH=ON -DRB_CURSOR_ENCODES_SCRATCH=ON
cmake --build build-cursor -j
```

The normal dense implementation remains the default.

## Validation

Before considering this representation for the main implementation, test:
1. repeated wrap-around over millions of publish/consume cycles;
2. full and empty transitions;
3. non-default limits;
4. every benchmark slot size;
5. ASan/UBSan builds;
6. producer/consumer threaded tests;
7. notification users, if this mode is intended to expose them;
8. ARM/Zynq builds, especially where 32-bit atomic operations are the target.

The representation is intentionally not combined with the per-slot-lap implementation in this experiment.
