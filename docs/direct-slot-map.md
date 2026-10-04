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

`RB_DIRECT_SLOT_MAP=1` and `RB_DIRECT_SLOT_MAP=2` are accepted only when:

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

Build identical benchmark configurations with `RB_DIRECT_SLOT_MAP=0`, `RB_DIRECT_SLOT_MAP=1`, and `RB_DIRECT_SLOT_MAP=2`.\n\nMode 2 is the control variant: it derives the slot directly like mode 1, but performs an explicit `volatile` load from `entries[tail & mask]` in `rb_consume()` and discards the value. This preserves the metadata-load cost without putting the loaded value in the slot-address dependency chain. Comparing modes 1 and 2 therefore tests whether the removed metadata load itself is responsible for the cache/latency effect. Mode 2 does not store to `entries[]` during publish, so it isolates the consumer-side metadata load.

Then inspect the generated assembly:

- `rb_publish_ex` should no longer contain the `entries[]` store.
- `rb_consume` should no longer contain the dependent `entries[]` load in mode 1.\n- Mode 2 should contain an explicit metadata load, but its result should not determine the scratch address.
- `rb_acquire` should remain functionally unchanged.

The benchmark result should be compared against the dense baseline rather than against the earlier sparse-layout experiment. The sparse experiment changes cache footprint; this experiment specifically tests removal of the metadata dependency.
## Four-mode isolation

The experiment now has four hot-path variants:

| Mode | Publish | Consume metadata load | Slot address |
|---|---|---|---|
| 0 | entries[] store | entries[] load | loaded metadata |
| 1 | none | none | direct from cursor |
| 2 | none | explicit volatile load | direct from cursor |
| 3 | entries[] store | explicit volatile load | direct from cursor |

Mode 2 retains the consumer-side metadata load while removing the publish store. Mode 3 retains both the publish store and consumer load, but the loaded value is deliberately not used to calculate the scratch address.

This gives three useful pairwise comparisons:

- **0 → 3:** retain store/load traffic, remove the metadata load's dependency on the scratch address.
- **3 → 2:** remove the publish-side entries[] store.
- **2 → 1:** remove the consumer-side metadata load.

The benchmark runner builds and measures all four modes independently. It also runs perf stat separately for each mode and writes the complete benchmark/performance-counter output to bench-results/perf-modes.md for easy copy/paste.


## Portable benchmark runner

`run-bench.sh` supports both bare-metal Linux hosts and virtualized CI runners.

On a host exposing Linux CPU-frequency policy files, the runner saves the original governor, switches to `performance`, disables Intel Turbo through `intel_pstate/no_turbo` when available, and restores the original settings on exit.

Virtualized runners may expose neither interface. The runner detects that case and records CPU-policy control as `unavailable` instead of failing; the four-mode benchmark and `perf stat` measurements continue.

This distinction matters when comparing results: local bare-metal measurements can be controlled for frequency and Turbo, while GitHub-hosted VM results are primarily useful for relative A/B and regression checks. Absolute latency numbers from the two environments are not directly comparable.
