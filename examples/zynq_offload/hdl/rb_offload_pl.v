// SPDX-License-Identifier: MIT
//
// rb dual-ring offload — Programmable Logic sketch for Zynq-7010 class
// (Antminer S9 control board / XC7Z010).
//
// Teaching design: BRAM sizing, doorbells, streaming hash.
// Not a drop-in, timing-closed bitstream.
//
// ---------------------------------------------------------------------------
// Resource budget (XC7Z010 ballpark)
// ---------------------------------------------------------------------------
//   Block RAM : ~240 Kb total
//   Default: SLOTS=16, SLOT_BYTES=256 → 2×4 KiB = 8 KiB scratchpads
//
// ---------------------------------------------------------------------------
// Address map (AXI-lite, relative; set AXIL_BASE in Vivado)
// ---------------------------------------------------------------------------
//   +0x00  DOORBELL_IN      W1S  CPU→PL   "new work on ring A"
//   +0x04  INGRESS_DONE     W1C  PL→CPU   "I have *read* ingress slot N"
//                                 → CPU should rb_release(ring_A, N)
//   +0x08  EGRESS_READY     W1C  PL→CPU   "result written on ring B"
//                                 → CPU should rb_consume(ring_B)
//   +0x0C  STATUS           RO   [0] busy, [1] ingress_done, [2] egress_ready
//   +0x10  LAST_IN_SLOT     RO   slot index last finished on ingress
//   +0x14  LAST_OUT_SLOT    RO   slot index last written on egress
//   +0x18  INGRESS_HEAD     RW   optional index mirrors
//   +0x1C  INGRESS_TAIL     RW
//   +0x20  EGRESS_HEAD      RW
//   +0x24  EGRESS_TAIL      RW
//   +0x28  CTRL             RW   [0] enable [1] irq_en [31] soft_reset
//
// IRQ (if USE_IRQ): level-high while (ingress_done | egress_ready) & irq_en
//
`timescale 1ns / 1ps

module rb_offload_pl #(
    parameter integer SLOTS       = 16,
    parameter integer SLOT_BYTES  = 256,
    parameter [31:0]  AXIL_BASE   = 32'h43C0_0000,
    parameter integer USE_BRAM    = 1,
    parameter integer USE_IRQ     = 1,
    parameter integer USE_HASH    = 1
) (
    input  wire         aclk,
    input  wire         aresetn,

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

    output wire         irq_out
);

    localparam integer ADDR_W = $clog2(SLOTS);

    // ---- doorbells / control ----
    reg        doorbell_in;    // CPU → PL
    reg        ingress_done;   // PL → CPU: finished *reading* ring A slot
    reg        egress_ready;   // PL → CPU: finished *writing* ring B slot
    reg        enable;
    reg        soft_reset;
    reg        irq_en;
    reg        busy;

    reg [31:0] last_in_slot;
    reg [31:0] last_out_slot;
    reg [31:0] ingress_head, ingress_tail;
    reg [31:0] egress_head,  egress_tail;

    assign irq_out = USE_IRQ ? ((ingress_done | egress_ready) & irq_en) : 1'b0;

    // ---- scratchpads ----
generate
    if (USE_BRAM) begin : gen_bram
        (* ram_style = "block" *) reg [7:0] scratch_a [0:SLOTS*SLOT_BYTES-1];
        (* ram_style = "block" *) reg [7:0] scratch_b [0:SLOTS*SLOT_BYTES-1];
    end else begin : gen_regs
        reg [7:0] scratch_a [0:SLOTS*SLOT_BYTES-1];
        reg [7:0] scratch_b [0:SLOTS*SLOT_BYTES-1];
    end
endgenerate

    // ---- AXI-lite write ----
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
            ingress_done <= 1'b0;
            egress_ready <= 1'b0;
            enable       <= 1'b0;
            soft_reset   <= 1'b0;
            irq_en       <= 1'b0;
            busy         <= 1'b0;
            last_in_slot  <= 0;
            last_out_slot <= 0;
            ingress_head <= 0;
            ingress_tail <= 0;
            egress_head  <= 0;
            egress_tail  <= 0;
        end else begin
            if (s_axi_awvalid && !have_aw) begin
                awaddr_r <= s_axi_awaddr;
                have_aw  <= 1'b1;
                s_axi_awready <= 1'b1;
            end else
                s_axi_awready <= 1'b0;

            if (s_axi_wvalid && !have_w) begin
                have_w <= 1'b1;
                s_axi_wready <= 1'b1;
            end else
                s_axi_wready <= 1'b0;

            if (have_aw && have_w && !s_axi_bvalid) begin
                case (awaddr_r[7:2])
                    6'h00: if (s_axi_wdata[0]) doorbell_in  <= 1'b1; // set
                    6'h01: if (s_axi_wdata[0]) ingress_done <= 1'b0; // W1C
                    6'h02: if (s_axi_wdata[0]) egress_ready <= 1'b0; // W1C
                    6'h06: ingress_head <= s_axi_wdata;
                    6'h07: ingress_tail <= s_axi_wdata;
                    6'h08: egress_head  <= s_axi_wdata;
                    6'h09: egress_tail  <= s_axi_wdata;
                    6'h0A: begin
                        enable     <= s_axi_wdata[0];
                        irq_en     <= s_axi_wdata[1];
                        soft_reset <= s_axi_wdata[31];
                    end
                    default: ;
                endcase
                have_aw <= 1'b0;
                have_w  <= 1'b0;
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;
            end
            if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;
        end
    end

    // ---- AXI-lite read ----
    always @(posedge aclk) begin
        if (!aresetn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 0;
            s_axi_rresp   <= 2'b00;
        end else if (s_axi_arvalid && !s_axi_rvalid) begin
            s_axi_arready <= 1'b1;
            s_axi_rvalid  <= 1'b1;
            s_axi_rresp   <= 2'b00;
            case (s_axi_araddr[7:2])
                6'h00: s_axi_rdata <= {31'd0, doorbell_in};
                6'h01: s_axi_rdata <= {31'd0, ingress_done};
                6'h02: s_axi_rdata <= {31'd0, egress_ready};
                6'h03: s_axi_rdata <= {29'd0, egress_ready, ingress_done, busy};
                6'h04: s_axi_rdata <= last_in_slot;
                6'h05: s_axi_rdata <= last_out_slot;
                6'h06: s_axi_rdata <= ingress_head;
                6'h07: s_axi_rdata <= ingress_tail;
                6'h08: s_axi_rdata <= egress_head;
                6'h09: s_axi_rdata <= egress_tail;
                6'h0A: s_axi_rdata <= {soft_reset, 29'd0, irq_en, enable};
                default: s_axi_rdata <= 32'hDEAD_BEEF;
            endcase
        end else begin
            s_axi_arready <= 1'b0;
            if (s_axi_rvalid && s_axi_rready)
                s_axi_rvalid <= 1'b0;
        end
    end

    // ---- pipeline: read A → hash → write B → dual completion ----
    // CPU still owns real rb_* in the recommended bring-up path.
    // PL raises:
    //   ingress_done  = "I have read the data"  → rb_release(A)
    //   egress_ready  = "result is available"   → rb_consume(B)

    localparam S_IDLE  = 3'd0;
    localparam S_HASH  = 3'd1;
    localparam S_STORE = 3'd2;
    localparam S_DONE  = 3'd3;

    reg [2:0]        state;
    reg [ADDR_W-1:0] slot_r;
    reg [15:0]       byte_idx;
    reg [31:0]       hash_r, len_r, seq_r;

    function [31:0] fnv_step;
        input [31:0] h;
        input [7:0]  b;
        begin
            fnv_step = (h ^ {24'd0, b}) * 32'h01000193;
        end
    endfunction

    always @(posedge aclk) begin
        if (!aresetn || soft_reset) begin
            state        <= S_IDLE;
            doorbell_in  <= 1'b0;
            ingress_done <= 1'b0;
            egress_ready <= 1'b0;
            busy         <= 1'b0;
            byte_idx     <= 0;
            hash_r       <= 32'h811c9dc5;
            len_r        <= 0;
            seq_r        <= 0;
            last_in_slot  <= 0;
            last_out_slot <= 0;
        end else if (enable) begin
            case (state)
                S_IDLE: begin
                    if (doorbell_in) begin
                        slot_r    <= ingress_tail[ADDR_W-1:0];
                        len_r     <= SLOT_BYTES - 4;
                        byte_idx  <= 4;
                        hash_r    <= 32'h811c9dc5;
                        seq_r     <= seq_r + 1;
                        busy      <= 1'b1;
                        state     <= USE_HASH ? S_HASH : S_STORE;
                    end
                end

                S_HASH: begin
                    if (USE_BRAM)
                        hash_r <= fnv_step(hash_r,
                            gen_bram.scratch_a[{slot_r, byte_idx[7:0]}]);
                    else
                        hash_r <= fnv_step(hash_r,
                            gen_regs.scratch_a[{slot_r, byte_idx[7:0]}]);

                    if (byte_idx + 1 >= SLOT_BYTES) begin
                        // Finished *reading* the ingress payload.
                        ingress_done  <= 1'b1;
                        last_in_slot  <= { {(32-ADDR_W){1'b0}}, slot_r };
                        state         <= S_STORE;
                        byte_idx      <= 0;
                    end else
                        byte_idx <= byte_idx + 1;
                end

                S_STORE: begin
                    // Pack zo_result into egress scratch_b[slot]
                    if (USE_BRAM) begin
                        gen_bram.scratch_b[{slot_r, 8'd0}]  <= seq_r[7:0];
                        gen_bram.scratch_b[{slot_r, 8'd1}]  <= seq_r[15:8];
                        gen_bram.scratch_b[{slot_r, 8'd2}]  <= seq_r[23:16];
                        gen_bram.scratch_b[{slot_r, 8'd3}]  <= seq_r[31:24];
                        gen_bram.scratch_b[{slot_r, 8'd4}]  <= hash_r[7:0];
                        gen_bram.scratch_b[{slot_r, 8'd5}]  <= hash_r[15:8];
                        gen_bram.scratch_b[{slot_r, 8'd6}]  <= hash_r[23:16];
                        gen_bram.scratch_b[{slot_r, 8'd7}]  <= hash_r[31:24];
                        gen_bram.scratch_b[{slot_r, 8'd8}]  <= len_r[7:0];
                        gen_bram.scratch_b[{slot_r, 8'd9}]  <= len_r[15:8];
                        gen_bram.scratch_b[{slot_r, 8'd10}] <= len_r[23:16];
                        gen_bram.scratch_b[{slot_r, 8'd11}] <= len_r[31:24];
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
                    doorbell_in   <= 1'b0;
                    egress_ready  <= 1'b1;   // "result is available"
                    last_out_slot <= { {(32-ADDR_W){1'b0}}, slot_r };
                    // Optional mirrors (CPU still does real rb_release/publish)
                    ingress_tail  <= ingress_tail + 1;
                    egress_head   <= egress_head + 1;
                    busy          <= 1'b0;
                    state         <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
