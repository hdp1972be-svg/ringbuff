# Cache-line layout guidance

The ring control block keeps producer-owned and consumer-owned counters on separate cache-line boundaries. If the scratchpad is placed immediately after the control block, the control block size must also end on a cache-line boundary to avoid sharing a line between ring metadata and the first scratch slot.

`rb_size(capacity)` is therefore the authoritative size to reserve for the control block. Applications that place the scratchpad directly after it should use:

```c
size_t ring_bytes = rb_size(capacity);
rb_t *rb = (rb_t *)base;
void *scratch = (uint8_t *)base + ring_bytes;
```

The returned size is rounded up to `RB_CACHE_LINE`. This makes the ring/scratch boundary safe when the base allocation itself is suitably aligned.

If ring and scratchpad are allocated independently, keep the scratchpad base aligned to `RB_CACHE_LINE` when practical. Slot-cacheline padding (`RB_SLOT_CACHELINE_PAD`) is a separate concern: it prevents adjacent scratch slots from sharing cache lines, but does not by itself fix the control-block/scratchpad boundary.

For IPC mappings, retain the existing cache-line rounding used by the IPC layout helpers. For multi-socket NUMA systems, also see `docs/DEPENDENCIES.md`: producer/consumer placement and scratchpad NUMA placement can dominate the cost of an otherwise cache-friendly SPSC ring.
