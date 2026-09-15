# Examples

## IPC ring buffer — two processes sharing a ring in POSIX shared memory

`ipc_writer` and `ipc_reader` demonstrate using `rb` as an
inter-process ring buffer. The ring control block and the scratchpad
both live in a single `shm_open` region. The writer publishes messages;
the reader drains them and verifies the sequence is intact.

No threads are used. Two independent OS processes coordinate through
shared memory alone.

### Layout

```
            /dev/shm/rb_ipc_demo  (mmap'd by both processes)
            ┌──────────────────────────────────────────────┐
            │  rb_t control block       (RB_CACHE_LINE)    │
            │  scratchpad               (RB_CACHE_LINE)    │
            └──────────────────────────────────────────────┘
                  ▲                              ▲
                  │                              │
           ipc_reader (mmap)              ipc_writer (mmap + init)
```

### Build

```bash
cmake -B build -DRB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run

Terminal 1:

```bash
./build/examples/ipc_reader
```

Terminal 2:

```bash
./build/examples/ipc_writer
```

The writer publishes 100,000 messages and exits. The reader drains them
and reports `gaps=0`.

### Cleanup

The writer deliberately does **not** unlink the shared memory, because
the reader may still be attached when the writer exits. After both
processes have terminated:

```bash
rm /dev/shm/rb_ipc_demo
```

If the writer fails with `File exists`, the segment is stale from a
previous run. Remove it and try again.

### What this example demonstrates

- The ring and scratchpad can live in any shared memory region.
- `rb_acquire` / `rb_publish` / `rb_consume` / `rb_release` all work
  unchanged across process boundaries.
- The scratchpad alignment requirement (`RB_CACHE_LINE`) is satisfied
  because `mmap` returns page-aligned memory, and the scratchpad offset
  is rounded up to the next cache line.
- Backpressure works across processes: when the reader is slow, the
  writer's `rb_acquire` returns `RB_ERR_FULL` and it retries.

### What this example does not demonstrate

- **Notification.** The reader polls every 50 µs. For production use,
  replace the poll with `rb_wait(rb, snapshot, timeout)` — the ring's
  `head` counter is in shared memory, and `futex_wait` on a
  `MAP_SHARED` address wakes cross-process. See `docs/API.md` for
  the correct snapshot-before-drain loop.
- **Process-shared eventfd.** For event-loop integration, an `eventfd`
  created by the writer can be passed to the reader via `fork`,
  `SCM_RIGHTS`, or a named pipe. That path is out of scope here.

### Sequence verification

The writer formats each message as `"msg #<seq> from pid <pid> at
<time>"`. The reader parses the sequence number and checks it is
exactly one more than the previous. Any gap, duplication, or reorder
would show up as `gaps > 0` in the reader's summary. Under normal
operation this should always be zero.

---

## Burst / overload demo — backpressure and drop policy

`ipc_burst_writer` and `ipc_slow_reader` demonstrate what happens when
the producer is faster than the consumer. The writer publishes in bursts
of N items, then sleeps for Q milliseconds. The reader simulates
per-item processing cost with `usleep`. Together they reproduce
sustained overload and burstiness.

### Three scenarios worth running

**Scenario 1 — reader keeps up (no backpressure).**

```bash
# Terminal 1
./build/examples/ipc_slow_reader -d 20 -v
# Terminal 2
./build/examples/ipc_burst_writer -b 64 -q 50 -n 200000 -v
```

Writer blocks rarely. Reader queue depth stays low. Both finish; reader
reports `gaps=0`.

**Scenario 2 — reader can't keep up, writer blocks (WAIT policy).**

```bash
# Terminal 1 — slow reader
./build/examples/ipc_slow_reader -d 2000 -v
# Terminal 2 — bursty writer
./build/examples/ipc_burst_writer -b 256 -q 20 -n 500000 -m wait -v
```

Reader queue depth stays near `limit` (64/64). Writer reports high
`full_waits` counts — one per 100 µs spent retrying. `gaps=0`, so no
data is lost. Producer is throttled to the consumer's rate.

**Scenario 3 — reader can't keep up, writer drops (DROP policy).**

```bash
# Terminal 1 — very slow reader
./build/examples/ipc_slow_reader -d 5000 -v
# Terminal 2 — same writer, drop mode
./build/examples/ipc_burst_writer -b 256 -q 20 -n 500000 -m drop -v
```

Writer reports `dropped` growing quickly. Reader reports `gaps > 0`
and `missing items > 0` — but no duplicates, no reorder, no corruption.
The drop is explicit and counted on both sides.

### What each number means

**Writer side:**

| Field | Meaning |
|---|---|
| `full_waits` | `rb_acquire` returned `RB_ERR_FULL` and the writer retried. In WAIT mode, counts retry attempts. |
| `dropped` | `rb_acquire` returned `RB_ERR_FULL` and DROP mode skipped the item. Reader will see a gap. |
| `queue=N/M` | Ring count at the time of the report. In overload, `N == M`. |

**Reader side:**

| Field | Meaning |
|---|---|
| `gaps` | Number of sequence discontinuities. Zero in WAIT mode. |
| `missing items` | Sum of all skipped sequence numbers. |
| `max queue depth` | Peak count observed. `limit` means the ring was saturated. |
| `samples: full` | Fraction of samples where the ring was at `limit`. High = sustained overload. |
| `samples: empty` | Fraction of samples where the ring was at 0. High = reader is underutilized. |

In Scenario 2, `samples: full` should be near 100%. In Scenario 1, it
should be near 0%.

### Cleanup

Both burst examples leave the segment in place; remove it manually:

```bash
rm -f /dev/shm/rb_ipc_demo
```

If the writer exits with `File exists`, a previous run left the segment
behind. Remove it and retry.

### What this demonstrates

- **WAIT policy** turns the ring into a proper backpressure boundary.
  The producer is slowed to the consumer's rate. No data is lost.
- **DROP policy** keeps the producer at full speed at the cost of
  explicit, counted data loss. The consumer sees gaps.
- **Edge-triggered `on_full` callbacks** would fire once per overload
  episode, not once per publish — see `docs/API.md` for the semantics.
- **The ring never loses items it accepted.** Every `rb_publish` that
  succeeded is matched by exactly one `rb_release`, in FIFO order,
  with the payload intact.
  
---

## Notification demo — consumer sleeps, producer wakes

`notify_threaded` runs a producer thread and a consumer loop in one
process. The consumer blocks in `rb_wait()` between bursts, using
~0% CPU while idle.

```bash
cmake -B build -DRB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/examples/notify_threaded
```

Expected output:

```
notification example: consumer blocks in rb_wait between bursts
done: produced=200 consumed=200 in ~2.5s
wakeups=10 idle_timeouts=~25 (timeouts = ~70% idle)
```

**The correct consumer loop is snapshot-before-drain:**

```c
uint32_t v = rb_notify_value(rb);   // snapshot head FIRST
if (rb_drain(rb, fn, user) > 0) continue;
rb_wait(rb, v, timeout);
```

Getting this order backwards loses wakeups under load. See
`docs/API.md` for the detailed reasoning.

**Cross-process note:** `rb_wait` uses `FUTEX_WAIT_PRIVATE`, which is
keyed on the process's `mm_struct`. It works between threads in one
process, not between processes sharing `MAP_SHARED` memory. For
cross-process notification, use `rb_notify_fd()` (eventfd) instead, or
a POSIX named semaphore.

---

## GPU shared scratchpad

`gpu_shared` allocates the ring's scratchpad with `cudaHostAlloc(...,
cudaHostAllocMapped)`. The same physical pages are addressable from the
CPU (host pointer) and the GPU (device pointer). No copy on the data
path.

```bash
cmake -B build -DRB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/examples/gpu_shared
```

Built only when CMake finds a CUDA toolkit. On systems without one, the
target is skipped with a `gpu_shared skipped (no CUDA toolkit)` message.

The example shows:

- `cudaHostAlloc` with `cudaHostAllocMapped` for pinned, mappable memory
- `cudaHostGetDevicePointer` to obtain the GPU-side address of the same pages
- The ring's control block in regular heap (the GPU never needs to see it)
- Producer writes payloads directly into slots
- A `cudaMemcpy` round trip demonstrating the data is visible from both sides

For AMD ROCm the equivalent calls are `hipHostMalloc(hipHostMallocMapped)`
and `hipHostGetDevicePointer`. The `rb` library is unchanged.

For a full Zynq flow (ARM + FPGA sharing DDR over AXI), the pattern is
the same: reserve a DDR region in the device tree, `mmap` it from both
sides, use `rb` on the ARM with `RB_CACHE_LINE` matching the AXI line
size, and let the PL read/write the slots through an AXI master.

---

## In-process backpressure with callbacks

`backpressure_inproc` runs a producer thread and a consumer loop in one
process. The producer is not blocked or spinning when the ring fills —
instead, the ring's **latched callbacks** set and clear a
`stop_reading` flag, and the producer checks that flag between items.

```bash
# WAIT policy: producer pauses, no data lost
./build/examples/backpressure_inproc

# DROP policy: producer keeps going, counts dropped items
./build/examples/backpressure_inproc -m drop
```

**WAIT mode output (approximate):**

```
produced=2000 dropped=0 consumed=2000 in ~0.4s
stats: high_water=32 full_hits=~60 low_d_hits=~60
```

**DROP mode output (approximate):**

```
produced=1200 dropped=800 consumed=1200 in ~0.4s
stats: high_water=32 full_hits=~60 low_d_hits=~60
```

**What the latched callback counts mean:**

- `full_hits ≈ 60` means the ring went from not-full to full about 60
  times. Not 2000, not 32. One event per overload episode.
- `low_d_hits ≈ 60` means the consumer drained below 30% about 60
  times, and each time cleared the producer's `stop_reading` flag.
- Both counters are roughly equal because the two events alternate:
  full sets the flag, low_d clears it. That's the hysteresis working.

If you disabled latching, `full_hits` would be closer to 2000 (one per
publish attempt at the boundary), and the callback would drown in
noise. See `docs/TECHNICAL.md` for the latching rationale.

**The scratchpad is plain `malloc`'d memory here.** On x86-64, `malloc`
returns 16-byte aligned memory, which satisfies `_Alignof(rb_t)` for
the ring, and the slot stride computed by `rb_init` rounds `slot_size`
up to `_Alignof(max_align_t)` (16 bytes on x86-64). No special
alignment is needed for this configuration. For embedded targets with
`RB_SLOT_CACHELINE_PAD=1`, use `posix_memalign` or a static
`_Alignas(RB_CACHE_LINE)` buffer instead.

---

## Zynq-7000 / Antminer S9 dual-ring offload

`examples/zynq_offload/` is a complete CPU ↔ FPGA offload sketch aimed at
Petalinux on the PS and programmable logic on the PL (Antminer S9 class
boards).

- **Ring A (ingress):** CPU publishes WebSocket JSON frames; FPGA consumes.
- **Ring B (egress):** FPGA publishes hashed/processed results; CPU drains.
- Scratchpads live in a shared region (POSIX shm for host testing, reserved
  DDR/BRAM on the real board).
- Uses the `RB_HW_FLUSH_SLOT` / `RB_HW_INVALIDATE_SLOT` / `RB_HW_NOTIFY_DEVICE`
  stubs for cache visibility and doorbells.

```bash
cmake -B build -DRB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target zynq_cpu_host zynq_fpga_stub

# Terminal 1
./build/examples/zynq_fpga_stub

# Terminal 2
./build/examples/zynq_cpu_host
```

See `examples/zynq_offload/README.md` for the memory map, Petalinux device-tree
notes, and how to replace the software stub with real AXI/BRAM logic.
