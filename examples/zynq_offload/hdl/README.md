# PL HDL sketch — `rb_offload_pl.v`

Parameterized Verilog for the FPGA side of the dual-ring offload on a
Zynq-7010 class device (Antminer S9 control board).

This is a **teaching design**: it shows BRAM sizing, doorbell addresses,
and a tiny streaming hash. It is not a complete, timing-closed bitstream.

## Compile-time parameters

| Parameter | Default | Meaning |
|-----------|---------|--------|
| `SLOTS` | 16 | Number of slots per scratchpad |
| `SLOT_BYTES` | 256 | Bytes per slot (incl. 4 B header) |
| `AXIL_BASE` | `0x43C00000` | Informational AXI-lite base (match Vivado) |
| `USE_BRAM` | 1 | 1 = Block RAM, 0 = regs (sim only) |
| `USE_IRQ` | 1 | Drive `irq_out` when egress doorbell is set |
| `USE_HASH` | 1 | 1 = FNV-style hash, 0 = skip to store |

### BRAM budget on XC7Z010

```
bytes_per_scratchpad = SLOTS * SLOT_BYTES
total_scratch        = 2 * bytes_per_scratchpad
```

Defaults: `16 * 256 = 4 KiB` each → **8 KiB** total ≈ a couple of 36 Kb
BRAMs. The Z-7010 has on the order of **~240 Kb** Block RAM; leave headroom
for AXI interconnect, your real compute IP, and FIFOs.

Examples:

| SLOTS | SLOT_BYTES | One pad | Both pads | Fit on 7010? |
|------:|-----------:|--------:|----------:|:-------------|
| 16 | 256 | 4 KiB | 8 KiB | yes, easy |
| 32 | 512 | 16 KiB | 32 KiB | yes |
| 64 | 512 | 32 KiB | 64 KiB | yes, watch other IP |
| 64 | 2048 | 128 KiB | 256 KiB | tight / no |

If you need larger payloads, prefer **reserved DDR** + AXI HP master
instead of on-chip BRAM (see main README).

## AXI-lite register map

Relative to `AXIL_BASE` (example `0x43C0_0000`):

| Offset | Name | Access | Role |
|--------|------|--------|------|
| `0x00` | `DOORBELL_IN` | W1C set | CPU → PL “new ingress work” |
| `0x04` | `DOORBELL_OUT` | W1C clear | PL → CPU “result ready” (also IRQ) |
| `0x08` | `STATUS` | RO | bit0 = busy |
| `0x0C` | `INGRESS_HEAD` | RW | mirror of ring A head |
| `0x10` | `INGRESS_TAIL` | RW | mirror of ring A tail |
| `0x14` | `EGRESS_HEAD` | RW | mirror of ring B head |
| `0x18` | `EGRESS_TAIL` | RW | mirror of ring B tail |
| `0x1C` | `CTRL` | RW | [0] enable, [1] irq_en, [31] soft reset |

Wire `irq_out` to a fabric interrupt input on the Zynq PS if `USE_IRQ=1`.

## How this lines up with the C side

1. CPU `rb_publish` on ring A + `RB_HW_NOTIFY_DEVICE` → writes `DOORBELL_IN`.
2. PL state machine hashes `scratch_a[slot]` and packs `zo_result` into
   `scratch_b[slot]`.
3. PL sets `DOORBELL_OUT` (and IRQ).
4. CPU handler clears `DOORBELL_OUT`, invalidates the egress slot,
   `rb_consume` / process / `rb_release` on ring B, and `rb_release` on
   ring A when appropriate.

The index mirrors are **optional helpers** for a PL-driven pipeline.
The safest first bring-up keeps real `rb_*` ownership on the CPU and
only uses the PL for payload transform + doorbells.

## Synthesis notes

- Set `USE_BRAM=1` for FPGA; `0` only for quick sim with tiny `SLOTS`.
- Mark scratchpad CPU mappings non-cacheable **or** keep the
  `RB_HW_FLUSH_SLOT` / `RB_HW_INVALIDATE_SLOT` paths in `hw_port.h`.
- Connect port B of each BRAM (or an AXI BRAM controller) so the PS can
  `memcpy` into the same bytes the PL reads.
- Replace the one-byte-per-cycle hash with a wider datapath or a real
  FFT/crypto IP when you outgrow the example.

## Simulation vs real FPGA

```text
Simulation:  USE_BRAM=0, small SLOTS, feed AXI-lite from a testbench
Real S9:     USE_BRAM=1, SLOTS/SLOT_BYTES sized to leftover BRAM,
             AXIL_BASE = address you assigned in Vivado Address Editor,
             irq_out → GIC, dual-port BRAM or DDR window shared with PS
```
