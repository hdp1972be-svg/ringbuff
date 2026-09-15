// SPDX-License-Identifier: MIT
//
// Reusable SPSC ring building blocks for the PL.
//
// These modules own head/tail the same way the C library does:
//   - producer advances head after writing a slot
//   - consumer advances tail after reading a slot
//   - full  when (head - tail) >= LIMIT
//   - empty when (head == tail)
//
// Indices are free-running uint32 (wrap naturally).  Slot address is
//   base + (index % SLOTS) * SLOT_BYTES
// or, if SLOTS is power-of-two, (index & (SLOTS-1)) * SLOT_BYTES.
//
// Latency note (order-of-magnitude on Zynq-7010 @ ~100–150 MHz):
//   A thin pipeline (doorbell → read N bytes → simple hash → write
//   result → completion) is often in the low-µs range per frame.
//   ~3 µs × pipeline depth is a reasonable planning figure for small
//   slots; measure on your bitstream before trusting it for trading.
//
`timescale 1ns / 1ps

// ---------------------------------------------------------------------------
// rb_spsc_ctrl — shared head/tail + occupancy helpers
// ---------------------------------------------------------------------------
module rb_spsc_ctrl #(
    parameter integer LIMIT = 16
) (
    input  wire        clk,
    input  wire        rst_n,

    // Producer side
    input  wire        prod_req,     // request a free slot
    output reg         prod_grant,
    output reg  [31:0] prod_index,   // absolute index to write
    input  wire        prod_done,    // producer finished write → publish

    // Consumer side
    input  wire        cons_req,     // request next occupied slot
    output reg         cons_grant,
    output reg  [31:0] cons_index,
    input  wire        cons_done,    // consumer finished read → release

    // Status
    output wire [31:0] head_o,
    output wire [31:0] tail_o,
    output wire [31:0] count_o,
    output wire        full_o,
    output wire        empty_o
);
    reg [31:0] head, tail;

    assign head_o  = head;
    assign tail_o  = tail;
    assign count_o = head - tail;
    assign full_o  = (head - tail) >= LIMIT[31:0];
    assign empty_o = (head == tail);

    always @(posedge clk) begin
        if (!rst_n) begin
            head       <= 0;
            tail       <= 0;
            prod_grant <= 1'b0;
            cons_grant <= 1'b0;
            prod_index <= 0;
            cons_index <= 0;
        end else begin
            prod_grant <= 1'b0;
            cons_grant <= 1'b0;

            // Acquire (producer)
            if (prod_req && !full_o) begin
                prod_index <= head;
                prod_grant <= 1'b1;
            end
            // Publish
            if (prod_done)
                head <= head + 1;

            // Consume
            if (cons_req && !empty_o) begin
                cons_index <= tail;
                cons_grant <= 1'b1;
            end
            // Release
            if (cons_done)
                tail <= tail + 1;
        end
    end
endmodule

// ---------------------------------------------------------------------------
// rb_slot_addr — map absolute index → byte offset in scratchpad
// ---------------------------------------------------------------------------
module rb_slot_addr #(
    parameter integer SLOTS      = 16,
    parameter integer SLOT_BYTES = 256
) (
    input  wire [31:0] index,
    output wire [31:0] byte_off,
    output wire [$clog2(SLOTS)-1:0] slot_id
);
    localparam integer SLOT_W = $clog2(SLOTS);
    // Power-of-two fast path
    generate
        if ((SLOTS & (SLOTS - 1)) == 0) begin : pow2
            assign slot_id  = index[SLOT_W-1:0];
            assign byte_off = { index[SLOT_W-1:0], { $clog2(SLOT_BYTES){1'b0} } };
        end else begin : general
            assign slot_id  = index % SLOTS;
            assign byte_off = (index % SLOTS) * SLOT_BYTES;
        end
    endgenerate
endmodule

// ---------------------------------------------------------------------------
// rb_pl_consumer — pull one slot from an ingress ring, pulse data_valid
// ---------------------------------------------------------------------------
// Downstream logic sees:
//   data_valid + slot_index  → read payload from scratchpad
//   data_ready               → assert when finished reading
// Module then advances tail (release).
//
module rb_pl_consumer #(
    parameter integer LIMIT = 16
) (
    input  wire        clk,
    input  wire        rst_n,

    // Tie to shared rb_spsc_ctrl consumer ports
    output reg         cons_req,
    input  wire        cons_grant,
    input  wire [31:0] cons_index,
    output reg         cons_done,

    // Datapath handshake
    output reg         data_valid,
    output reg  [31:0] slot_index,
    input  wire        data_ready   // downstream: "I have read the data"
);
    localparam S_IDLE = 2'd0;
    localparam S_WAIT = 2'd1;
    localparam S_HOLD = 2'd2;
    localparam S_REL  = 2'd3;
    reg [1:0] state;

    always @(posedge clk) begin
        if (!rst_n) begin
            state      <= S_IDLE;
            cons_req   <= 1'b0;
            cons_done  <= 1'b0;
            data_valid <= 1'b0;
            slot_index <= 0;
        end else begin
            cons_req  <= 1'b0;
            cons_done <= 1'b0;
            case (state)
                S_IDLE: begin
                    cons_req <= 1'b1;
                    state    <= S_WAIT;
                end
                S_WAIT: begin
                    if (cons_grant) begin
                        slot_index <= cons_index;
                        data_valid <= 1'b1;
                        state      <= S_HOLD;
                    end else
                        state <= S_IDLE; // empty — retry
                end
                S_HOLD: begin
                    if (data_ready) begin
                        data_valid <= 1'b0;
                        state      <= S_REL;
                    end
                end
                S_REL: begin
                    cons_done <= 1'b1; // advance tail
                    state     <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase
        end
    end
endmodule

// ---------------------------------------------------------------------------
// rb_pl_producer — get a free egress slot, pulse space_valid
// ---------------------------------------------------------------------------
// Upstream logic sees:
//   space_valid + slot_index → write payload into scratchpad
//   space_done               → assert when write finished
// Module then advances head (publish).
//
module rb_pl_producer #(
    parameter integer LIMIT = 16
) (
    input  wire        clk,
    input  wire        rst_n,

    output reg         prod_req,
    input  wire        prod_grant,
    input  wire [31:0] prod_index,
    output reg         prod_done,

    output reg         space_valid,
    output reg  [31:0] slot_index,
    input  wire        space_done
);
    localparam S_IDLE = 2'd0;
    localparam S_WAIT = 2'd1;
    localparam S_HOLD = 2'd2;
    localparam S_PUB  = 2'd3;
    reg [1:0] state;

    always @(posedge clk) begin
        if (!rst_n) begin
            state       <= S_IDLE;
            prod_req    <= 1'b0;
            prod_done   <= 1'b0;
            space_valid <= 1'b0;
            slot_index  <= 0;
        end else begin
            prod_req  <= 1'b0;
            prod_done <= 1'b0;
            case (state)
                S_IDLE: begin
                    prod_req <= 1'b1;
                    state    <= S_WAIT;
                end
                S_WAIT: begin
                    if (prod_grant) begin
                        slot_index  <= prod_index;
                        space_valid <= 1'b1;
                        state       <= S_HOLD;
                    end else
                        state <= S_IDLE; // full — retry
                end
                S_HOLD: begin
                    if (space_done) begin
                        space_valid <= 1'b0;
                        state       <= S_PUB;
                    end
                end
                S_PUB: begin
                    prod_done <= 1'b1; // advance head
                    state     <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase
        end
    end
endmodule
