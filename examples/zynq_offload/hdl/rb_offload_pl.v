// SPDX-License-Identifier: MIT
//
// rb dual-ring offload — Programmable Logic sketch for Zynq-7010 class
// (Antminer S9 control board / XC7Z010).
//
// This is a *teaching* design, not a drop-in bitstream.  It shows:
//   - how much BRAM you need for the two scratchpads,
//   - where doorbell / status registers sit on the AXI-lite bus,
//   - a tiny streaming hash that consumes ingress slots and produces
//     egress results,
//   - compile-time switches for real vs simulation builds.
//
// ---------------------------------------------------------------------------
// Resource budget (XC7Z010 ballpark)
// ---------------------------------------------------------------------------
//   Block RAM : ~240 Kb total (~60 × 36 Kb primitives, tool-dependent)
//   DSP48     : ~80
//
// Default geometry in this file:
//   SLOTS      = 16
//   SLOT_BYTES = 256
//   → one scratchpad = 16 * 256 = 4 KiB = 32 Kib
//   → two scratchpads = 8 KiB = 64 Kib  (~1–2 BRAM36s each, fine)
//
// Raise SLOTS/SLOT_BYTES only while 2 * SLOTS * SLOT_BYTES still fits
// comfortably in remaining BRAM after your other IP.
//
// ---------------------------------------------------------------------------
// Address map (AXI-lite, example — change to match your Vivado design)
// ---------------------------------------------------------------------------
//   BASE + 0x00  DOORBELL_IN     W1C  CPU→PL  "new ingress work"
//   BASE + 0x04  DOORBELL_OUT    W1C  PL→CPU  "new egress result" (also IRQ)
//   BASE + 0x08  STATUS          RO   busy / error bits
//   BASE + 0x0C  INGRESS_HEAD    RW   software-visible copy of ring A head
//   BASE + 0x10  INGRESS_TAIL    RW   … tail (PL advances on consume)
//   BASE + 0x14  EGRESS_HEAD     RW   ring B head (PL advances on publish)
//   BASE + 0x18  EGRESS_TAIL     RW   … tail
//   BASE + 0x1C  CTRL            RW   soft reset, enable, IRQ enable
//
// Scratchpad physical addresses are *not* on this lite bus.  They are
// either:
//   (A) dual-port BRAM instantiated here, port A = PL pipeline,
//       port B = AXI BRAM controller from the PS, or
//   (B) a window into reserved DDR that an AXI HP master in this module
//       reads/writes (more flexible, higher latency).
//
// This sketch implements (A) for clarity.
//
`timescale 1ns / 1ps

module rb_offload_pl #(
    // ---- geometry (keep 2*SLOTS*SLOT_BYTES inside your BRAM budget) ----
    parameter integer SLOTS       = 16,
    parameter integer SLOT_BYTES  = 256,
    parameter integer SLOT_W      = SLOT_BYTES * 8,

    // ---- AXI-lite base (informational; decoder is relative) ----
    // Example: 0x43C0_0000 is a common GP0 offset in Zynq designs.
    parameter [31:0] AXIL_BASE    = 32'h43C0_0000,

    // ---- compile switches ----
    // 1 = infer BRAM for both scratchpads (real FPGA / synth)
    // 0 = use plain regs (simulation only — will not fit if SLOTS large)
    parameter integer USE_BRAM    = 1,

    // 1 = generate a PL→PS interrupt when DOORBELL_OUT is set
    parameter integer USE_IRQ     = 1,

    // 1 = include the tiny FNV-style hash pipeline
    // 0 = loopback payload length only (bring-up)
    parameter integer USE_HASH    = 1
) (
    input  wire         aclk,
    input  wire         aresetn,

    // AXI4-Lite slave (doorbell / status / ring index mirrors)
    input  wire [7:0]   s_axi_awaddr,
    input  wire         s_axi_awvalid,
    output reg          s_axi_awready,
    input  wire [31:0]  s_axi_wdata,
    input  wire [3:0]   s_axi_wstrb,
    input  wire         s_axi_wvalid,
    output reg          s_axi_wready,
    output reg  [1:0]   s_axi_bresp,
    output reg          s_axi_bvalid,
    input  wire         s_axi_bready,
    input  wire [7:0]   s_axi_araddr,
    input  wire         s_axi_arvalid,
    output reg          s_axi_arready,
    output reg  [31:0]  s_axi_rdata,
    output reg  [1:0]   s_axi_rresp,
    output reg          s_axi_rvalid,
    input  wire         s_axi_rready,

    // Optional interrupt to GIC (active-high level)
    output wire         irq_out
);

    // ------------------------------------------------------------------
    // Localparams derived from geometry
    // ------------------------------------------------------------------
    localparam integer ADDR_W     = $clog2(SLOTS);
    localparam integer BYTE_ADDR_W = $clog2(SLOTS * SLOT_BYTES);

    // ------------------------------------------------------------------
    // Doorbell / control registers
    // ------------------------------------------------------------------
    reg        doorbell_in;   // CPU wrote 1 → PL has work
    reg        doorbell_out;  // PL wrote 1 → CPU has result
    reg        enable;
    reg        soft_reset;
    reg        irq_en;
    reg [31:0] status;

    // Software-visible ring cursors (mirrors; real rb still owns protocol
    // on the CPU side in the recommended bring-up path).
    reg [31:0] ingress_head, ingress_tail;
    reg [31:0] egress_head,  egress_tail;

    assign irq_out = USE_IRQ ? (doorbell_out & irq_en) : 1'b0;

    // ------------------------------------------------------------------
    // Scratchpad memories
    // ------------------------------------------------------------------
    // Byte-addressable view for the pipeline.  Synthesis maps these to
    // Block RAM when USE_BRAM=1.
    //
    // Depth = SLOTS * SLOT_BYTES  (e.g. 16*256 = 4096 bytes).
    //
generate
    if (USE_BRAM) begin : gen_bram
        (* ram_style = "block" *) reg [7:0] scratch_a [0:SLOTS*SLOT_BYTES-1];
        (* ram_style = "block" *) reg [7:0] scratch_b [0:SLOTS*SLOT_BYTES-1];
    end else begin : gen_regs
        reg [7:0] scratch_a [0:SLOTS*SLOT_BYTES-1];
        reg [7:0] scratch_b [0:SLOTS*SLOT_BYTES-1];
    end
endgenerate

    // Convenience: which array we actually use
    // (hierarchical reference into the generate block)
    // For readability the pipeline below uses macros.

    // ------------------------------------------------------------------
    // AXI-lite write path (simplified, single outstanding)
    // ------------------------------------------------------------------
    reg [7:0] awaddr_r;
    reg       have_aw, have_w;

    always @(posedge aclk) begin
        if (!aresetn || soft_reset) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            s_axi_bresp   <= 2'b00;
            have_aw <= 1'b0;
            have_w  <= 1'b0;
            doorbell_in  <= 1'b0;
            doorbell_out <= 1'b0;
            enable       <= 1'b0;
            soft_reset   <= 1'b0;
            irq_en       <= 1'b0;
            ingress_head <= 0;
            ingress_tail <= 0;
            egress_head  <= 0;
            egress_tail  <= 0;
            status       <= 0;
        end else begin
            // Accept address
            if (s_axi_awvalid && !have_aw) begin
                awaddr_r      <= s_axi_awaddr;
                have_aw       <= 1'b1;
                s_axi_awready <= 1'b1;
            end else begin
                s_axi_awready <= 1'b0;
            end

            // Accept data
            if (s_axi_wvalid && !have_w) begin
                have_w       <= 1'b1;
                s_axi_wready <= 1'b1;
            end else begin
                s_axi_wready <= 1'b0;
            end

            // Commit write
            if (have_aw && have_w && !s_axi_bvalid) begin
                case (awaddr_r[7:2])
                    6'h00: begin // DOORBELL_IN W1C set
                        if (s_axi_wdata[0]) doorbell_in <= 1'b1;
                    end
                    6'h01: begin // DOORBELL_OUT W1C clear by CPU
                        if (s_axi_wdata[0]) doorbell_out <= 1'b0;
                    end
                    6'h03: ingress_head <= s_axi_wdata;
                    6'h04: ingress_tail <= s_axi_wdata;
                    6'h05: egress_head  <= s_axi_wdata;
                    6'h06: egress_tail  <= s_axi_wdata;
                    6'h07: begin // CTRL
                        enable     <= s_axi_wdata[0];
                        irq_en     <= s_axi_wdata[1];
                        soft_reset <= s_axi_wdata[31];
                    end
                    default: ;
                endcase
                have_aw      <= 1'b0;
                have_w       <= 1'b0;
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;
            end

            if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;
        end
    end

    // ------------------------------------------------------------------
    // AXI-lite read path
    // ------------------------------------------------------------------
    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 0;
            s_axi_rresp   <= 2'b00;
        end else begin
            if (s_axi_arvalid && !s_axi_rvalid) begin
                s_axi_arready <= 1'b1;
                s_axi_rvalid  <= 1'b1;
                s_axi_rresp   <= 2'b00;
                case (s_axi_araddr[7:2])
                    6'h00: s_axi_rdata <= {31'd0, doorbell_in};
                    6'h01: s_axi_rdata <= {31'd0, doorbell_out};
                    6'h02: s_axi_rdata <= status;
                    6'h03: s_axi_rdata <= ingress_head;
                    6'h04: s_axi_rdata <= ingress_tail;
                    6'h05: s_axi_rdata <= egress_head;
                    6'h06: s_axi_rdata <= egress_tail;
                    6'h07: s_axi_rdata <= {soft_reset, 29'd0, irq_en, enable};
                    default: s_axi_rdata <= 32'hDEAD_BEEF;
                endcase
            end else begin
                s_axi_arready <= 1'b0;
                if (s_axi_rvalid && s_axi_rready)
                    s_axi_rvalid <= 1'b0;
            end
        end
    end

    // ------------------------------------------------------------------
    // Tiny streaming pipeline (hash one ingress slot → one egress result)
    // ------------------------------------------------------------------
    // Recommended bring-up: CPU still owns rb_publish/rb_release.
    // PL only:
    //   - watches doorbell_in / index mirrors,
    //   - reads bytes from scratch_a[slot],
    //   - writes result struct into scratch_b[slot],
    //   - sets doorbell_out.
    //
    // Result layout (matches common.h zo_result):
    //   [0:3]   seq
    //   [4:7]   hash
    //   [8:11]  in_len
    //   [12:15] "HASH"

    localparam S_IDLE  = 3'd0;
    localparam S_LOAD  = 3'd1;
    localparam S_HASH  = 3'd2;
    localparam S_STORE = 3'd3;
    localparam S_DONE  = 3'd4;

    reg [2:0]  state;
    reg [ADDR_W-1:0] slot_r;
    reg [15:0] byte_idx;
    reg [31:0] hash_r;
    reg [31:0] len_r;
    reg [31:0] seq_r;

    // FNV-1a style (same spirit as the C stub)
    function [31:0] fnv_step;
        input [31:0] h;
        input [7:0]  b;
        begin
            fnv_step = (h ^ {24'd0, b}) * 32'h01000193;
        end
    endfunction

    integer bi;

    always @(posedge aclk) begin
        if (!aresetn || soft_reset) begin
            state        <= S_IDLE;
            doorbell_in  <= 1'b0;
            doorbell_out <= 1'b0;
            status       <= 0;
            byte_idx     <= 0;
            hash_r       <= 32'h811c9dc5;
            len_r        <= 0;
            seq_r        <= 0;
        end else if (enable) begin
            case (state)
                S_IDLE: begin
                    if (doorbell_in) begin
                        // In a fuller design, read length from the 4-byte
                        // slot header the CPU wrote.  Here we assume a
                        // fixed demo length for clarity.
                        slot_r   <= ingress_tail[ADDR_W-1:0];
                        len_r    <= SLOT_BYTES - 4; // skip header
                        byte_idx <= 4;              // payload starts after hdr
                        hash_r   <= 32'h811c9dc5;
                        seq_r    <= seq_r + 1;
                        state    <= USE_HASH ? S_HASH : S_STORE;
                        status[0] <= 1'b1; // busy
                    end
                end

                S_HASH: begin
                    // One byte per cycle — easy to follow; widen later.
                    if (USE_BRAM) begin
                        hash_r <= fnv_step(hash_r,
                            gen_bram.scratch_a[{slot_r, byte_idx[7:0]}]);
                    end else begin
                        hash_r <= fnv_step(hash_r,
                            gen_regs.scratch_a[{slot_r, byte_idx[7:0]}]);
                    end
                    if (byte_idx + 1 >= SLOT_BYTES) begin
                        state    <= S_STORE;
                        byte_idx <= 0;
                    end else begin
                        byte_idx <= byte_idx + 1;
                    end
                end

                S_STORE: begin
                    // Pack zo_result into scratch_b[slot]
                    // seq
                    if (USE_BRAM) begin
                        gen_bram.scratch_b[{slot_r, 8'd0}]  <= seq_r[7:0];
                        gen_bram.scratch_b[{slot_r, 8'd1}]  <= seq_r[15:8];
                        gen_bram.scratch_b[{slot_r, 8'd2}]  <= seq_r[23:16];
                        gen_bram.scratch_b[{slot_r, 8'd3}]  <= seq_r[31:24];
                        // hash
                        gen_bram.scratch_b[{slot_r, 8'd4}]  <= hash_r[7:0];
                        gen_bram.scratch_b[{slot_r, 8'd5}]  <= hash_r[15:8];
                        gen_bram.scratch_b[{slot_r, 8'd6}]  <= hash_r[23:16];
                        gen_bram.scratch_b[{slot_r, 8'd7}]  <= hash_r[31:24];
                        // in_len
                        gen_bram.scratch_b[{slot_r, 8'd8}]  <= len_r[7:0];
                        gen_bram.scratch_b[{slot_r, 8'd9}]  <= len_r[15:8];
                        gen_bram.scratch_b[{slot_r, 8'd10}] <= len_r[23:16];
                        gen_bram.scratch_b[{slot_r, 8'd11}] <= len_r[31:24];
                        // tag "HASH"
                        gen_bram.scratch_b[{slot_r, 8'd12}] <= "H";
                        gen_bram.scratch_b[{slot_r, 8'd13}] <= "A";
                        gen_bram.scratch_b[{slot_r, 8'd14}] <= "S";
                        gen_bram.scratch_b[{slot_r, 8'd15}] <= "H";
                    end else begin
                        gen_regs.scratch_b[{slot_r, 8'd0}]  <= seq_r[7:0];
                        gen_regs.scratch_b[{slot_r, 8'd1}]  <= seq_r[15:8];
                        gen_regs.scratch_b[{slot_r, 8'd2}]  <= seq_r[23:16];
                        gen_regs.scratch_b[{slot_r, 8'd3}]  <= seq_r[31:24];
                        gen_regs.scratch_b[{slot_r, 8'd4}]  <= hash_r[7:0];
                        gen_regs.scratch_b[{slot_r, 8'd5}]  <= hash_r[15:8];
                        gen_regs.scratch_b[{slot_r, 8'd6}]  <= hash_r[23:16];
                        gen_regs.scratch_b[{slot_r, 8'd7}]  <= hash_r[31:24];
                        gen_regs.scratch_b[{slot_r, 8'd8}]  <= len_r[7:0];
                        gen_regs.scratch_b[{slot_r, 8'd9}]  <= len_r[15:8];
                        gen_regs.scratch_b[{slot_r, 8'd10}] <= len_r[23:16];
                        gen_regs.scratch_b[{slot_r, 8'd11}] <= len_r[31:24];
                        gen_regs.scratch_b[{slot_r, 8'd12}] <= "H";
                        gen_regs.scratch_b[{slot_r, 8'd13}] <= "A";
                        gen_regs.scratch_b[{slot_r, 8'd14}] <= "S";
                        gen_regs.scratch_b[{slot_r, 8'd15}] <= "H";
                    end
                    state <= S_DONE;
                end

                S_DONE: begin
                    doorbell_in  <= 1'b0; // consume CPU doorbell
                    doorbell_out <= 1'b1; // signal CPU
                    // Advance local mirrors (CPU still does real rb_release)
                    ingress_tail <= ingress_tail + 1;
                    egress_head  <= egress_head + 1;
                    status[0]    <= 1'b0;
                    state        <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
