# PL HDL sketch — `rb_offload_pl.v`

Parameterized Verilog for the FPGA side of the dual-ring offload on a
Zynq-7010 class device (Antminer S9 control board).

Teaching design: BRAM sizing, **split completion doorbells**, streaming hash.

## Why two PL→CPU signals?

A single “doorbell out” is ambiguous. In a real dual-ring offload you need:

| Signal | Meaning | CPU action |
|--------|---------|------------|
| **INGRESS_DONE** | “I have **read** slot *N* on ring A” | `rb_release(ring_A, N)` → free the slot |
| **EGRESS_READY** | “I have **written** a result on ring B” | `rb_consume(ring_B)` → take the result |

These can fire at different times (read finishes before write, or vice versa
in a deeper pipeline). The IRQ line is the OR of both (when enabled).

## Compile-time parameters

| Parameter | Default | Meaning |
|-----------|---------|--------|
| `SLOTS` | 16 | Slots per scratchpad |
| `SLOT_BYTES` | 256 | Bytes per slot |
| `AXIL_BASE` | `0x43C00000` | Informational AXI-lite base |
| `USE_BRAM` | 1 | Block RAM vs regs (sim) |
| `USE_IRQ` | 1 | `irq_out = (ingress_done \| egress_ready) & irq_en` |
| `USE_HASH` | 1 | FNV hash vs store-only |

### BRAM budget (XC7Z010 ~240 Kb)

| SLOTS | SLOT_BYTES | Both pads | Fit? |
|------:|-----------:|----------:|:-----|
| 16 | 256 | 8 KiB | easy |
| 32 | 512 | 32 KiB | yes |
| 64 | 512 | 64 KiB | watch other IP |
| 64 | 2048 | 256 KiB | too tight |

## AXI-lite register map

Relative to `AXIL_BASE`:

| Offset | Name | Access | Role |
|--------|------|--------|------|
| `0x00` | `DOORBELL_IN` | W1S | CPU → PL: new ingress work |
| `0x04` | `INGRESS_DONE` | W1C | PL → CPU: finished **reading** slot |
| `0x08` | `EGRESS_READY` | W1C | PL → CPU: result **written** |
| `0x0C` | `STATUS` | RO | `[0] busy [1] ingress_done [2] egress_ready` |
| `0x10` | `LAST_IN_SLOT` | RO | slot index last read on A |
| `0x14` | `LAST_OUT_SLOT` | RO | slot index last written on B |
| `0x18` | `INGRESS_HEAD` | RW | optional mirrors |
| `0x1C` | `INGRESS_TAIL` | RW | |
| `0x20` | `EGRESS_HEAD` | RW | |
| `0x24` | `EGRESS_TAIL` | RW | |
| `0x28` | `CTRL` | RW | `[0] enable [1] irq_en [31] soft_reset` |

## CPU-side handler sketch

```c
/* After IRQ or poll */
if (readl(BASE + 0x04) & 1) {          /* INGRESS_DONE */
    uint32_t slot = readl(BASE + 0x10); /* LAST_IN_SLOT */
    rb_release(ring_a, slot);
    writel(1, BASE + 0x04);            /* W1C clear */
}
if (readl(BASE + 0x08) & 1) {          /* EGRESS_READY */
    /* rb_consume(ring_b, ...) then process result */
    writel(1, BASE + 0x08);
}
```

`RB_HW_NOTIFY_DEVICE` on the CPU publish path writes `DOORBELL_IN` (`BASE+0x00`).

## Pipeline order in this sketch

1. CPU publishes → `DOORBELL_IN`
2. PL hashes bytes from `scratch_a` → raises **`INGRESS_DONE`** ("I have read the data")
3. PL packs `zo_result` into `scratch_b` → raises **`EGRESS_READY`**
4. CPU releases A and consumes B (order flexible; usually release soon for back-pressure)

## Simulation vs real FPGA

```text
Simulation:  USE_BRAM=0, small SLOTS, testbench drives AXI-lite
Real S9:     USE_BRAM=1, size to leftover BRAM,
             AXIL_BASE from Vivado Address Editor,
             irq_out → GIC, dual-port BRAM shared with PS
```
