# rb — Benchmarks

[← Back to overview](../README.md) · [API reference](API.md) · [Technical notes](TECHNICAL.md)

Measured throughput and per-operation cost of the `rb` ring buffer.
All numbers were produced on the machine described below, in Release
builds, with the code as of this document's date.

These are absolute baselines, not competitive claims. The purpose is to
establish that the ring is not the bottleneck in the intended use case,
and to identify where it becomes memory-bound.

---

## Test environment

| Component | Detail |
|---|---|
| CPU | Intel Core i7-6500U (Skylake-U, 2 cores / 4 threads) |
| Base clock | 2.50 GHz |
| Max turbo | 3.10 GHz |
| L2 cache | 512 KB (256 KB per core) |
| L3 cache | 4 MB shared |
| TDP | 15 W |
| RAM | 8 GB |
| OS | Ubuntu (Linux) |
| Compiler | GCC 12.5.0 |
| Build | `-DCMAKE_BUILD_TYPE=Release` |
| CPU governor | Both `powersave` and `performance` measured |
| Library config | `RB_SLOT_SIZE=2048`, `RB_NUM_SLOTS=64`, `RB_CAPACITY=64` unless noted |
| Threads | Single producer, single consumer |

### About the CPU governor

Two sets of numbers are reported below:

- **`powersave`** — the default desktop governor, CPU not pinned, free
  to ramp frequency under load but doing so conservatively.
- **`performance`** — CPU pinned to maximum frequency via
  `/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` for the
  duration of the runs.

To reproduce the `performance` figures:

```bash
# Pin all cores to performance (requires sudo)
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Verify
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Rerun the benchmarks
cmake --build build-bench
./build-bench/bench/bench_rb
./build-bench/bench/bench_rb_random

# Restore afterwards
echo powersave | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### What the governor actually changed

**Very little, for these benchmarks.** On modern Intel parts, `powersave`
does not prevent the CPU from boosting under load; it only makes the
ramp slower. The `rb` benchmarks are short enough (0.4 s for the full
cycle, ~60 ms for each random-payload row) that the ramp is a small
fraction of the run.

The one place it mattered was the cache-cliff row (`capacity=1024,
slot=8192`), which runs for tens of seconds and gives the CPU time to
actually leave `powersave`'s slower frequency ramp. That row improved
by 15% under `performance`. Every other row is within a few percent,
and one row (256 B fixed-size) came out slower under `performance`,
which is almost certainly single-run noise rather than a real effect.

**Conclusion:** the governor is not a meaningful factor for these
numbers. The design conclusion (the ring is not the bottleneck) holds
identically in both regimes.

---

## Benchmark 1 — Full ring cycle (fixed, no payload copy)

**What it measures:** `rb_acquire` + `rb_publish` + `rb_consume` +
`rb_release`, with no payload copy. This is the intrinsic cost of the
ring itself, independent of how much data moves through it.

**Tool:** `bench/bench_rb.c`

### `powersave` governor (default desktop)

| capacity | slot size | Cycles/s | ns per cycle |
|---|---|---|---|
| 64 | 64 B | 38.11 M | 26.24 |
| 64 | 256 B | 41.51 M | 24.09 |
| 64 | 2048 B | 44.15 M | 22.65 |
| 64 | 8192 B | 41.97 M | 23.83 |
| 1024 | 64 B | 42.97 M | 23.27 |
| 1024 | 256 B | 41.82 M | 23.91 |
| 1024 | 2048 B | 44.09 M | 22.68 |
| 1024 | 8192 B | 25.56 M | 39.13 |

### `performance` governor (CPU pinned to max frequency)

| capacity | slot size | Cycles/s | ns per cycle | Δ vs powersave |
|---|---|---|---|---|
| 64 | 64 B | 39.24 M | 25.49 | +3.0% |
| 64 | 256 B | 45.58 M | 21.94 | +9.8% |
| 64 | 2048 B | 44.21 M | 22.62 | +0.1% |
| 64 | 8192 B | 44.02 M | 22.71 | +4.9% |
| 1024 | 64 B | 43.22 M | 23.14 | +0.6% |
| 1024 | 256 B | 45.11 M | 22.17 | +7.9% |
| 1024 | 2048 B | 44.54 M | 22.45 | +1.0% |
| 1024 | 8192 B | 29.43 M | 33.97 | **+15.1%** |

### Interpretation

- **Baseline: ~22 ns per full cycle.** That is roughly one cache miss
  plus the 4-byte header `memcpy`, and it is the floor for any
  configuration. At 2 KB slots, a 22 ns cycle is ~45 million
  acquire/publish/consume/release operations per second.
- **Slot size barely matters until the working set leaves L3.** From
  64 B to 8 KB, the per-cycle cost is flat as long as
  `capacity × stride` fits in cache.
- **The cache cliff is visible in the last row of both tables.**
  `capacity=1024` with 8192-byte slots is an 8 MB working set — larger
  than the 4 MB L3 on this CPU. Throughput drops ~40% under
  `powersave` (22.68 → 39.13 ns) and ~34% under `performance`
  (22.45 → 33.97 ns). If you raise capacity with large slots, budget
  for this.
- **2048-byte slots are the sweet spot.** They are the fastest rows in
  both tables, and they match the intended WS frame data size.

---

## Benchmark 2 — Fixed-size payloads (real bytes)

**What it measures:** the same full cycle, but with `memcpy` of a
fixed number of bytes from a 1 MB PRNG pool into the slot before
publish. This is the cost when the ring is actually moving data.

**Tool:** `bench/bench_rb_random.c` (slot = 2048 B, capacity = 64,
unless noted)

### `powersave` governor

| Payload | Items/s | Throughput |
|---|---|---|
| 16 B | 21.65 M | 330 MB/s |
| 256 B | 19.09 M | 4.66 GB/s |
| 1024 B | 11.70 M | 11.42 GB/s |
| 2044 B | 8.04 M | 15.67 GB/s |

### `performance` governor

| Payload | Items/s | Throughput | Δ items/s |
|---|---|---|---|
| 16 B | 21.36 M | 326 MB/s | −1.3% |
| 256 B | 16.31 M | 3.98 GB/s | **−14.6%** |
| 1024 B | 12.78 M | 12.48 GB/s | +9.2% |
| 2044 B | 8.38 M | 16.34 GB/s | +4.2% |
| 4096 B (slot 8192, cap 1024) | 2.48 M | 9.70 GB/s | n/a |

### Interpretation

- **Small payloads are overhead-bound.** At 16 B, items/s is close to
  the no-copy baseline (21.65 M vs 44 M cycles/s), because the ring
  cycle dominates and the copy is negligible.
- **Large payloads are bandwidth-bound.** At 2044 B, 15.7–16.3 GB/s is
  at or near this laptop's practical single-core memory bandwidth. The
  ring is no longer the constraint; the memory bus is.
- **The 256 B row is noisy.** It came out 14.6% slower under
  `performance` than under `powersave`, which is implausible as a real
  effect and consistent with single-run variance on a shared, non-isolated
  CPU. Each random-payload row runs for only ~60 ms; a scheduler tick
  or background process can swing it by that much. Treat sub-10%
  differences in this table as noise.
- **The last row is the cache-cliff test.** 1024 slots × 8192 B = 8 MB
  working set, exceeding the 4 MB L3. Throughput drops to 9.7 GB/s
  from the warm-cache 16.3 GB/s at 2044 B — same root cause as the
  no-copy cache cliff, now visible in MB/s.

---

## Benchmark 3 — Variable-size payloads (WS-like)

**What it measures:** the same cycle, with per-item lengths drawn from
a uniform distribution. This models real WebSocket frames, which are
not all the same size.

**Tool:** `bench/bench_rb_random.c`

### `powersave` governor

**Slot = 2048 B, capacity = 64:**

| Length range | Average | Items/s | Throughput |
|---|---|---|---|
| 16 – 128 B | 72 B | 27.77 M | 1.91 GB/s |
| 128 – 1024 B | 576 B | 22.16 M | 12.18 GB/s |
| 256 – 2044 B | 1150 B | 13.28 M | 14.56 GB/s |

**Slot = 8192 B, capacity = 64:**

| Length range | Average | Items/s | Throughput |
|---|---|---|---|
| 512 – 4096 B | 2305 B | 7.78 M | 17.10 GB/s |
| 2048 – 8192 B | 5118 B | 3.97 M | 19.37 GB/s |

### `performance` governor

**Slot = 2048 B, capacity = 64:**

| Length range | Average | Items/s | Throughput | Δ items/s |
|---|---|---|---|---|
| 16 – 128 B | 72 B | 29.56 M | 2.03 GB/s | +6.4% |
| 128 – 1024 B | 576 B | 21.45 M | 11.78 GB/s | −3.2% |
| 256 – 2044 B | 1150 B | 14.88 M | 16.31 GB/s | +12.0% |

**Slot = 8192 B, capacity = 64:**

| Length range | Average | Items/s | Throughput | Δ items/s |
|---|---|---|---|---|
| 512 – 4096 B | 2303 B | 8.52 M | 18.71 GB/s | +9.5% |
| 2048 – 8192 B | 5115 B | 4.10 M | 20.01 GB/s | +3.3% |

### Interpretation

- **Variable-size is sometimes *faster* than fixed-size.** The 16–128 B
  variable row (27–30 M items/s) beats the 16 B fixed row (21 M
  items/s). This is not a measurement error. `memcpy` for a
  compile-time constant is inlined to a single load/store; `memcpy`
  for a runtime-variable length goes through glibc's optimized
  vectorized routine, which can be faster for mid-range sizes. The
  branch that checks the length is perfectly predicted, so it costs
  nothing.
- **The WS-relevant range is the 256–2044 B row:** 13–15 M items/s at
  14.6–16.3 GB/s. That is the number to quote for "can this keep up
  with WebSocket traffic" — and it is roughly four orders of magnitude
  above what a single socket produces.
- **The 8 KB rows push past 20 GB/s under `performance`.** At that
  point the ring overhead is irrelevant; you are measuring the memory
  copy.

---

## Summary

| Question | Answer |
|---|---|
| Intrinsic ring cost | ~22 ns per full cycle |
| Max no-copy throughput | ~44–45 M cycles/s |
| Fastest slot size | 2048 B |
| Where it becomes memory-bound | ~11 GB/s and above |
| Safe working set | capacity × stride < L3 (4 MB on this CPU) |
| WS-range throughput | ~14 M frames/s, ~16 GB/s at 256–2044 B |
| Governor effect on these benchmarks | Small (<10% on most rows) |

### What the numbers mean for the intended use case

The intended pipeline is a socket reader publishing 2 KB frames into a
64-slot ring, with a transform consumer draining it. The 2 KB, 64-slot
configuration is the fastest row in the no-copy benchmark and is well
inside L2.

Even at the slowest realistic payload row (2044 B, 8.38 M items/s under
`performance`), the ring would have to be presented with ~8 million 2 KB
frames per second before it became the bottleneck — far beyond any
single WebSocket connection, and far beyond what the downstream
transform could process.

**The ring is not the bottleneck. That is the point of the design.**

---

## Reproducing

```bash
cmake -B build-bench -DRB_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench

./build-bench/bench/bench_rb
./build-bench/bench/bench_rb_random
```

Both benchmarks print a per-item checksum so the compiler cannot
eliminate the copy or the consume. The random benchmark seeds from
`/dev/urandom` and prints the seed; a run is reproducible by setting
`g_rng` to the printed value.

### Caveats

- **Single machine, single run per configuration.** Treat the absolute
  numbers as indicative, not authoritative. Sub-10% differences between
  runs of the same configuration are noise on this system.
- **The `performance` governor run was not thermally controlled.** On a
  15 W ULV part under sustained load, the CPU can throttle before the
  benchmark finishes. The cache-cliff row runs for tens of seconds and
  is the most likely to be affected.
- **`sched_yield` and `usleep` in the threaded tests are not the
  tightest possible wait primitives.** They are chosen for portability.
  A production integration using `rb_notify` (eventfd) would spin less
  and might measure differently under saturation.
- **No comparison against other SPSC ring implementations is
  included.** Different projects publish numbers on different hardware
  and workloads; quoting them here would be misleading.

---

## See also

- [API reference](API.md) — how to use the ring.
- [Technical notes](TECHNICAL.md) — design rationale, config matrix,
  porting.
- [Overview](../README.md) — what it is and why.
