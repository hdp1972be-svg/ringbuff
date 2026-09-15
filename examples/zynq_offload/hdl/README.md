# PL HDL — dual-ring offload + SPSC building blocks

## Files

| File | Role |
|------|------|
| `rb_spsc.v` | **Reusable** SPSC blocks: ctrl, consumer, producer, slot address |
| `rb_offload_pl.v` | Example top: BRAM + AXI-lite doorbells + hash (CPU-owned or PL-owned) |
| `README.md` | This file |

---

## Complete offload building blocks (`rb_spsc.v`)

These modules let the **PL own head/tail** the same way the C library does,
so the FPGA can be a true consumer of ring A and producer of ring B with no
CPU in the data plane.

| Module | Role |
|--------|------|
| `rb_spsc_ctrl` | Shared `head` / `tail`, grant free or occupied slots, advance on done |
| `rb_slot_addr` | Absolute index → byte offset / slot id (pow2 fast path) |
| `rb_pl_consumer` | Reqath handshake: `data_valid` / `data_ready` then **release** (tail++) |
| `rb_pl_producer` | Datapath handshake: `space_valid` / `space_done` then **publish** (head++) |

### Wire pattern (ingress ring A inside PL)

```text
  rb_spsc_ctrl (LIMIT = capacity)
       │
       ├── cons_*  →  rb_pl_consumer  →  data_valid/slot  →  your hash/FFT
       │                                      data_ready ←  "I have read the data"
       │
       └── (CPU may still peek head/tail via AXI if desired)
```

### Wire pattern (egress ring B inside PL)

```text
  your compute  →  space_valid path via rb_pl_producer  →  write scratch_b
                         space_done when store complete  →  head++
```

### Handshake meaning (matches earlier doorbell semantics)

| Event | Block | Meaning |
|-------|--------|--------|
| Consumer asserts after read | `data_ready` | “I have **read** the data” → `cons_done` → tail++ |
| Producer asserts after write | `space_done` | “Result is **written**” → `prod_done` → head++ |

Optional AXI-lite can still expose `INGRESS_DONE` / `EGRESS_READY` as **wakeups**
for the CPU even when the PL advances indices itself.

---

## Latency planning figure (~3 µs × depth)

On a Zynq-7010 PL clock around **100–150 MHz**:

- Simple byte-iterating hash over a few hundred bytes is often **1–5 µs**
- Thin control overhead (grant → datapath → done) is usually **tens of cycles**
- End-to-end (CPU publish → PL done → CPU see result) is often in the
  **low single-digit microseconds** for small slots if doorbells/IRQs are tight

So **~3 µs × pipeline stages** is a reasonable *planning* number for this
class of board and slot size — not a guarantee. Measure on the real bitstream
(cycle counters or GPIO toggle) before using it in a trading path.

Larger slots, DDR instead of BRAM, or deeper FFT IP will push latency up.

---

## Doorbells when CPU still owns the rings (`rb_offload_pl.v`)

If you keep software `rb_*` ownership for bring-up:

| Offset | Name | Meaning |
|--------|------|--------|
| `+0x00` | `DOORBELL_IN` | CPU → PL: new work |
| `+0x04` | `INGRESS_DONE` | PL → CPU: finished **reading** → `rb_release(A)` |
| `+0x08` | `EGRESS_READY` | PL → CPU: result **written** → `rb_consume(B)` |
| `+0x10` | `LAST_IN_SLOT` | |
| `+0x14` | `LAST_OUT_SLOT` | |
| `+0x28` | `CTRL` | enable / irq_en / soft_reset |

IRQ = `(ingress_done | egress_ready) & irq_en`.

---

## BRAM budget (XC7Z010 ~240 Kb)

| SLOTS | SLOT_BYTES | Both pads |
|------:|-----------:|----------:|
| 16 | 256 | 8 KiB |
| 32 | 512 | 32 KiB |
| 64 | 512 | 64 KiB |

---

## Two bring-up paths

1. **CPU owns rings** — use `rb_offload_pl` doorbells only; C does all `rb_*`.
2. **PL owns rings** — instantiate `rb_spsc_ctrl` + `rb_pl_consumer` +
   `rb_pl_producer` per ring; CPU only feeds ingress DRAM/BRAM and drains
   egress (or is removed from the hot path entirely).

Path 2 is “complete offload.” Path 1 is safer for first light on the S9.
