# Zynq-7000 / Antminer S9 dual-ring offload

Real-world example: CPU (Petalinux on the PS) pushes WebSocket JSON
or market-data frames into an **ingress** ring whose scratchpad lives in
shared memory (DDR visible to the PL, or on-chip BRAM). The FPGA reads
the slots, runs a fast transform (hash, light crypto, rolling stats,
FFT-style work), and writes results into an **egress** ring. The CPU
drains the egress ring.

```
  NIC / WS / market data
           │
           ▼
  ┌─────────────────┐   ring A (CPU → FPGA)    ┌──────────────────────┐
  │  CPU (PS)       │ ───────────────────────► │  FPGA (PL)           │
  │  Petalinux      │   scratchpad in DDR/BRAM │  hash / FFT / encrypt│
  │  dual Cortex-A9 │ ◄─────────────────────── │  indicator crunch    │
  └─────────────────┘   ring B (FPGA → CPU)    └──────────────────────┘
           │
           ▼
  results / hashed frames / indicators
```

Both rings are ordinary `rb` instances. The only platform-specific
parts are:

1. Where the scratchpads live (shared DDR window or BRAM).
2. The `RB_HW_FLUSH_SLOT` / `RB_HW_INVALIDATE_SLOT` / `RB_HW_NOTIFY_DEVICE`
   overrides (cache maintenance + doorbell).
3. How the FPGA is told “new work” and how the CPU is told “result ready”
   (AXI-lite doorbell or interrupt).

---

## Target board: Antminer S9 control board

The S9 control board is a cheap, widely available Zynq-7000 platform
originally built to drive hashboards. Stripped of mining duty it is a
capable little PS+PL board.

### SoC and memory

| Item | Typical S9 control board |
|------|---------------------------|
| SoC | Xilinx **Zynq-7010** (`XC7Z010-1CLG400C`) |
| CPU | Dual ARM Cortex-A9 @ **~667 MHz** |
| On-chip RAM | 256 KB OCM |
| External DRAM | **512 MB DDR3** common (some boards 256 MB or 1 GB) |
| Flash | **256 MB NAND** |
| Ethernet | Gigabit (Broadcom PHY on many boards) |
| Boot | NAND (stock), SD card via jumpers, JTAG |
| Extra | SD slot, JTAG header, UART pads, 2× buttons, LEDs |

### FPGA fabric (XC7Z010, approximate)

| Resource | Rough count |
|----------|-------------|
| Logic cells | ~28k |
| LUT / FF | ~17k / ~35k |
| Block RAM | ~240 KB (many 36 Kb blocks) |
| DSP48 slices | ~80 |
| PL ↔ PS | AXI HP/GP ports, interrupts, shared DDR |

That is enough for small streaming pipelines: rolling hashes, light
crypto, modest FFTs, EWMA / indicator filters, or a zip-style reducer.
It is **not** a large Ultrascale; keep algorithms lean and prefer
streaming over huge working sets.

### What the stock board does *not* give you

- No HDMI / USB / rich GPIO headers out of the box (community HATs exist).
- PL fabric is modest — budget BRAM and DSPs carefully.
- Original bitstream talks to hashboards over UART/I2C-style links; you
  replace that with your own design.

Community projects (PYNQ images, Armbian ports, Astra_S9, etc.) already
treat the S9 control board as a general Zynq-7010 dev board. Use those
as the base OS if you do not want a full Petalinux rebuild.

---

## Files in this example

| File | Role |
|------|------|
| `common.h` | Dual-ring layout, slot payload format, doorbell words |
| `hw_port.h` | Example overrides of the three `RB_HW_*` macros |
| `cpu_host.c` | PS-side program (producer on A, consumer on B) |
| `fpga_stub.c` | Userspace model of the PL path (host testing) |
| `README.md` | This file |

---

## Host test (no board)

```bash
cmake -B build -DRB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target zynq_cpu_host zynq_fpga_stub

# Terminal 1 – FPGA model
./build/examples/zynq_fpga_stub

# Terminal 2 – CPU host
./build/examples/zynq_cpu_host
```

Both processes share one POSIX shm region that holds the two control
blocks and the two scratchpads. This validates ownership, flush/
invalidate hooks, and doorbells before you touch hardware.

---

## What to adapt for a real S9 / Petalinux board

### 1. Shared memory instead of `shm_open`

On the board the scratchpads must sit in memory the PL can address:

**Option A — reserved DDR window (simplest for larger buffers)**

Device-tree fragment (addresses are examples; match your memory map):

```dts
reserved-memory {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges;

    rb_shared: buffer@0x1F000000 {
        compatible = "shared-dma-pool";
        reg = <0x1F000000 0x00100000>; /* 1 MiB */
        no-map;
    };
};
```

From Linux, map it with a small UIO driver, `/dev/mem` (if allowed), or
a custom char device. Pass the same physical base to the PL via AXI.

**Option B — on-chip BRAM (lowest latency, smaller capacity)**

Instantiate dual-port BRAM in Vivado, connect one port to an AXI BRAM
controller (PS) and the other to your PL pipeline. Size the rings so
`2 × slots × slot_size` fits in the BRAM you allocate (often tens of KB).

### 2. Replace the three hardware stubs

In `hw_port.h` (or a board-specific header):

```c
/* After CPU wrote the slot, before publish becomes visible to PL */
#define RB_HW_FLUSH_SLOT(rb, idx) do { \
    void *p = rb_slot_ptr((rb), (idx)); \
    Xil_DCacheFlushRange((UINTPTR)p, ZO_SLOT_SIZE); \
    dsb(); \
} while (0)

/* After PL wrote the slot, before CPU reads it */
#define RB_HW_INVALIDATE_SLOT(rb, idx) do { \
    void *p = rb_slot_ptr((rb), (idx)); \
    Xil_DCacheInvalidateRange((UINTPTR)p, ZO_SLOT_SIZE); \
    dsb(); \
} while (0)

/* Kick the PL */
#define RB_HW_NOTIFY_DEVICE(rb, idx, len, trunc) do { \
    (void)(rb); (void)(idx); (void)(len); (void)(trunc); \
    writel(1, fpga_doorbell_reg); /* AXI-lite */ \
} while (0)
```

If the scratchpad is **non-cacheable** (marked in the MMU / device tree),
flush/invalidate can become simple compiler barriers. Prefer that for
small high-rate rings when you can spare the attribute.

### 3. Completion path (PL → CPU)

Pick one:

| Method | Notes |
|--------|--------|
| AXI-lite status + IRQ | PL raises interrupt; ISR or thread calls `rb_release` on ring A and/or signals ring B readiness |
| Polled doorbell word | Same as the host stub (`fpga_to_cpu` flag); simple, higher latency |
| Soft-core in PL | MicroBlaze/etc. runs a tiny loop that does the `rb_*` calls itself (only if you put the control block in PL-visible memory and accept the complexity) |

Recommended for first bring-up: **IRQ → kernel/userspace handler → `rb_release` / drain ring B**. Keep a single consumer context per ring.

### 4. FPGA design sketch (PL)

Minimal streaming pipeline:

1. **AXI master** (or BRAM port) reads the next ingress slot when the
   doorbell fires or a free-running poller sees a new index.
2. **Compute block**: rolling hash, AES round, small FFT, EWMA, etc.
   Stream through DSP48s; keep state in BRAM registers.
3. **Write result** into the next egress slot.
4. **Advance ownership**: either the PL writes the ring indices itself
   (advanced) or it only fills payload + sets a completion flag and the
   CPU performs `rb_release` / `rb_publish` (safer first step).

Resource budget on XC7Z010 is tight. Prefer fixed-point, streaming,
and reuse of DSP/BRAM over large soft CPUs.

### 5. Software split on the PS

| Role | Runs on |
|------|--------|
| NIC / WS / protocol parse | Cortex-A9 (Linux) |
| `rb_publish` into ring A | same |
| Optional strategy / order logic | same, after draining ring B |
| Heavy math | PL only |

Keep the CPU out of the inner number-crunch loop. That is the whole
point of the dual-ring offload.

### 6. Build / deploy notes

- Cross-compile `cpu_host` (and any small helper) with the Petalinux or
  community toolchain for `arm-linux-gnueabihf` / aarch32.
- Ship a bitstream that exposes:
  - AXI window onto the shared scratchpads (or BRAM),
  - doorbell registers,
  - optional IRQ line to the GIC.
- Boot: SD or NAND with a rootfs that has your binary and the bitstream
  loaded early (U-Boot / fpga manager).

---

## Data path (one frame)

```
CPU                          shared DDR/BRAM                      FPGA
───                          ──────────────                       ────
rb_acquire(ring_A)
write payload into slot
RB_HW_FLUSH_SLOT             (cache clean + dsb)
rb_publish(ring_A)
RB_HW_NOTIFY_DEVICE  ──────► doorbell / IRQ
                                                      rb_consume(ring_A)
                                                      RB_HW_INVALIDATE_SLOT
                                                      hash / FFT / …
                                                      rb_acquire(ring_B)
                                                      write result
                                                      RB_HW_FLUSH_SLOT
                                                      rb_publish(ring_B)
                                                      notify CPU
rb_consume(ring_B)   ◄────── (IRQ or flag)
RB_HW_INVALIDATE_SLOT
use result
rb_release(ring_B)
                                                      rb_release(ring_A)
```

---

## Example “crunch” in this tree

The software stub computes a 32-bit FNV-style hash and returns:

```c
struct zo_result {
    uint32_t seq;
    uint32_t hash;
    uint32_t in_len;
    char     tag[4];   /* "HASH" */
};
```

On the real PL, replace that with whatever fits the DSP/BRAM budget:
streaming FFT bins, indicator vector, encrypted blob, compressed chunk,
etc. The ring protocol stays the same.

---

## Realistic expectations on an S9-class board

**Good fit**

- Low-latency market-data / WS ingest on the ARM side
- Small streaming transforms in the PL (hash, filter, modest FFT,
  rolling stats, light crypto)
- Deterministic hand-off via two SPSC rings
- Power- and cost-efficient “indicator accelerator” next to a larger
  trading stack elsewhere

**Tight / careful**

- Large FFTs or heavy matrix work (BRAM + DSP limits)
- Many parallel channels (one dual-ring pair is the natural unit;
  scale by instantiating more pairs if fabric allows)
- Cache-coherency mistakes (always flush/invalidate or use non-cacheable
  mappings for the scratchpads)

**Not the goal of this example**

- Replacing a full HFT NIC + FPGA appliance
- Running a complete exchange matching engine in the PL

The S9 board is a **teaching and prototype** platform: real PS+PL,
real shared memory, real interrupts, dirt-cheap. The same dual-ring +
`RB_HW_*` pattern ports to larger Zynq Ultrascale or pure FPGA cards
when you outgrow the 7010 fabric.
