# Zynq-7000 / Antminer S9 dual-ring offload

Real-world example: CPU (Petalinux on the PS) pushes WebSocket JSON
payloads into an **ingress** ring whose scratchpad lives in shared
memory (BRAM or DDR visible to the PL). The FPGA reads the slots,
performs a fast transform (hash / light encryption / zip-style
reduction), and writes results into an **egress** ring. The CPU drains
the egress ring.

```
  NIC / WS JSON
       │
       ▼
  ┌─────────────┐   ring A (CPU → FPGA)    ┌──────────────────┐
  │  CPU (PS)   │ ───────────────────────► │  FPGA (PL)       │
  │  Petalinux  │   scratchpad in BRAM/DDR │  hash / encrypt  │
  │             │ ◄─────────────────────── │  or stream-zip   │
  └─────────────┘   ring B (FPGA → CPU)    └──────────────────┘
       │
       ▼
  results / hashed frames
```

Both rings are ordinary `rb` instances. The only platform-specific
parts are:

1. Where the scratchpads live (shared BRAM or reserved DDR).
2. The `RB_HW_FLUSH_SLOT` / `RB_HW_INVALIDATE_SLOT` / `RB_HW_NOTIFY_DEVICE`
   overrides (cache maintenance + doorbell).
3. How the FPGA is told “new work” and how the CPU is told “result ready”
   (AXI doorbell registers or an interrupt).

## Files

| File | Role |
|------|------|
| `common.h` | Dual-ring layout, slot payload format, doorbell offsets |
| `hw_port.h` | Example overrides of the three `RB_HW_*` macros for Zynq |
| `cpu_host.c` | Petalinux-side program (producer on A, consumer on B) |
| `fpga_stub.c` | Userspace model of the FPGA path (for host testing) |
| `README.md` | This file |

## Build (host test without a board)

The stub lets you exercise the whole ownership + visibility path on any
Linux machine:

```bash
cmake -B build -DRB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target zynq_cpu_host zynq_fpga_stub

# Terminal 1 – FPGA model
./build/examples/zynq_offload/zynq_fpga_stub

# Terminal 2 – CPU host
./build/examples/zynq_offload/zynq_cpu_host
```

Both processes attach to the same POSIX shared-memory region that holds
the two control blocks and the two scratchpads.

## Build for the Antminer S9 / Petalinux

1. Cross-compile with your Petalinux SDK toolchain.
2. Map a reserved DDR region (or BRAM) in the device tree, e.g.:

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

3. In `cpu_host.c` replace the `shm_open` path with `mmap` of that
   physical region (via `/dev/mem` or a small UIO/driver).
4. Point the FPGA AXI master at the same physical addresses.
5. Override the three macros in `hw_port.h` with real Zynq barriers and
   your doorbell register writes (see comments in that file).

## Data path (one frame)

```
CPU                          shared memory                     FPGA
───                          ─────────────                     ────
rb_acquire(ring_A)
write WS-JSON into slot
RB_HW_FLUSH_SLOT             (cache clean + dsb)
rb_publish(ring_A)
RB_HW_NOTIFY_DEVICE  ──────► doorbell / IRQ
                                                     rb_consume(ring_A)
                                                     RB_HW_INVALIDATE_SLOT
                                                     hash / encrypt
                                                     rb_acquire(ring_B)
                                                     write result
                                                     RB_HW_FLUSH_SLOT
                                                     rb_publish(ring_B)
                                                     RB_HW_NOTIFY_DEVICE
rb_consume(ring_B)   ◄────── (IRQ or polled flag)
RB_HW_INVALIDATE_SLOT
use result
rb_release(ring_B)
                                                     rb_release(ring_A)
```

## What the FPGA “crunch” does in this example

The stub (and the suggested HDL) computes a simple 32-bit rolling hash
over the payload and writes back:

```
struct result {
    uint32_t seq;
    uint32_t hash;
    uint32_t in_len;
    uint8_t  tag[4];   /* "HASH" */
};
```

Replace the hash with AES, SHA-256, a streaming zip compressor, etc.
The ring protocol stays identical.

## Notes specific to the S9 / Zynq-7000

- PS and PL share the same DDR; BRAM is also dual-port capable.
- Use `dsb(sy)` / `dmb(sy)` (or the GCC builtins) in the HW stubs.
- Keep the ring **control blocks** in a coherent region if possible;
  only the scratchpad slots need the flush/invalidate dance.
- One outstanding `rb_consume` per ring still applies — the FPGA logic
  (or the stub thread) is the single consumer of ring A and the single
  producer of ring B.
