# Direct slot mapping experiment

This experiment removes the `entries[]` mapping from the default acquire/publish/consume hot path.

## Hypothesis

With `RB_USE_POINTERS=0`, `RB_PER_SLOT_LAP=0`, and `slots == capacity`, `rb_acquire()` already derives the physical scratch slot from the producer cursor. Therefore publication does not need to store that slot index in `entries[]`, and consumption can derive it from the consumer cursor instead of loading `entries[]`.

Expected hot-path change:

```text
publish: head -> entries[] store -> head publication
                         ^ removed

consume: tail -> entries[] load -> slot calculation
                         ^ removed
```

## Constraints

`RB_DIRECT_SLOT_MAP=1` is accepted only when:

- `RB_USE_POINTERS == 0`
- `RB_PER_SLOT_LAP == 0`
- runtime `slots == capacity`

`rb_init()` rejects other configurations. This is important because the normal API permits the logical ring capacity and physical scratch-slot count to differ; `entries[]` is then the mapping between those two domains.

## ABI isolation

The `entries[]` allocation is deliberately retained and `rb_size()` is unchanged. The first experiment therefore isolates the hot-path mapping cost without simultaneously changing the control-block layout or allocation size.

## Pointer and per-slot-lap modes

`RB_USE_POINTERS=1` is not included because pointer entries have semantics that cannot be replaced by an index calculation without changing the representation.

`RB_PER_SLOT_LAP=1` is also untouched. It has its own per-slot sequence protocol and does not use the default `entries[]` mapping in the same way.

## Validation

Build identical benchmark configurations with `RB_DIRECT_SLOT_MAP=0` and `RB_DIRECT_SLOT_MAP=1`.

Then inspect the generated assembly:

- `rb_publish_ex` should no longer contain the `entries[]` store.
- `rb_consume` should no longer contain the dependent `entries[]` load.
- `rb_acquire` should remain functionally unchanged.

The benchmark result should be compared against the dense baseline rather than against the earlier sparse-layout experiment. The sparse experiment changes cache footprint; this experiment specifically tests removal of the metadata dependency.