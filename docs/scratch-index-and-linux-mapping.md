# Scratch-canonical indices, exact mappings, and HugeTLB

The legacy API remains unchanged:

- `rb_init()` initializes the control block and canonical head/tail in `rb_t`.
- `RB_INDEX_MODE_LOCAL` is the default.
- Existing applications do not need to change.
- `rb_size()`, slot layout, callbacks, and the normal slot-pointer API retain their legacy meaning.

## Scratch-canonical head/tail

Set the mode explicitly:

```c
rb_config_t cfg;
rb_config_init(&cfg);
rb_config_set_index_mode(&cfg, RB_INDEX_MODE_SCRATCH);
rb_init(&rb, &cfg, scratch, scratch_size);
```

In this mode the scratchpad starts with two dedicated cache lines:

| Offset | Meaning |
|---:|---|
| 0 | canonical producer head |
| `RB_CACHE_LINE` | canonical consumer tail |
| `2 * RB_CACHE_LINE` | first payload slot |

The canonical words are C11 atomic uint32 values when atomics are enabled. The control block keeps CPU-side cached head/tail values. Producer and consumer ownership remains SPSC: the producer owns head and the consumer owns tail.

Scratch mode therefore does **not** require the device to understand the `rb_t` control block.

### Attaching a second CPU-side handle

`rb_init()` is the creator operation: it initializes the shared canonical indices.

When another process/thread needs its own local `rb_t` over the same scratchpad, use:

```c
rb_err_t e = rb_attach(&local_rb, &cfg, shared_scratch, scratch_size);
```

`rb_attach()` does not reset the shared head/tail. This is important for IPC and for a CPU/FPGA design where the scratchpad is the shared state.

The application is responsible for ensuring that exactly one creator initializes an empty ring before any consumer/attacher starts using it.

## Alignment

Scratch-canonical mode requires the scratch base to be at least `RB_CACHE_LINE` aligned, even when slot cache-line padding is disabled. This guarantees that head and tail occupy independent cache lines.

## Volatile payload access

The normal API remains non-volatile for maximum portability and performance. When software must explicitly express that payload memory is device-visible, use:

```c
volatile void *p = rb_slot_vptr(&rb, slot);
const volatile void *q = rb_slot_cvptr(&rb, slot);
```

`volatile` is a compiler-access qualifier. It is **not** a cache flush, DMA synchronization primitive, or CPU/device memory barrier. Non-coherent FPGA/DMA designs must still provide the appropriate `RB_HW_FLUSH_SLOT`, `RB_HW_INVALIDATE_SLOT`, and/or platform memory barriers in `rb_port.h`.

## Exact virtual mapping

The Linux IPC benchmark accepts:

```text
--address 0x7f0000000000
```

When supplied, the examples use `MAP_FIXED_NOREPLACE`. The address is a **virtual address**. It does not select a physical BRAM address.

`MAP_FIXED_NOREPLACE` is deliberately used instead of `MAP_FIXED`: an existing mapping is never silently destroyed. The requested address must also satisfy the mapping's page alignment requirements.

## HugeTLB benchmark

The IPC benchmark also accepts:

```text
--hugepages
```

This uses an actual HugeTLB-backed file under `/dev/hugepages/rb_ipc_bench`, not transparent huge pages and not `madvise(MADV_HUGEPAGE)`. The mapping is rounded up to the configured system HugeTLB page size reported by `/proc/meminfo`.

A HugeTLB pool must already be configured. If the machine has no suitable HugeTLB pages or no `/dev/hugepages` mount, the option fails instead of silently falling back to normal pages.

A small logical ring can therefore consume a complete huge page. That is intentional: this mode is a performance experiment, not a memory-efficiency mode.

## Backward compatibility

All new functionality is opt-in:

- default index mode remains `RB_INDEX_MODE_LOCAL`;
- `rb_init()` keeps its original signature and behavior;
- exact mapping is only used when `--address` is supplied;
- HugeTLB is only used with `--hugepages`;
- the volatile pointer API is additive;
- scratch-index mode rejects combinations that cannot safely derive slot ownership (`RB_USE_POINTERS` and `RB_PER_SLOT_LAP`).

For FPGA/device integration, the recommended architecture is a local `rb_t` per CPU-side participant plus one shared/device-visible scratchpad. The FPGA needs only the documented scratch layout and ownership protocol; it does not need the private CPU control block.
