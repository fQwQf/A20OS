`timescale 1ns/1ps
// One nonmultiplexed asynchronous 16-bit FSMC port. NWAIT is mandatory.
// Payload is bundled with the synchronized active strobe, not synchronized
// bit by bit: the master must hold it stable from settling through response.
module a20_fsmc_bridge #(
    parameter int CAPTURE_CYCLES = 4
)(
    input logic clk, reset_n,
    input logic ne_ram_n, ne_control_n, noe_n, nwe_n,
    input logic [24:0] address,
    input logic [1:0] nbl_n,
    input logic [15:0] dq_in,
    output logic [15:0] dq_out,
    output logic dq_oe, nwait,
    output logic req_valid, req_control, req_write,
    output logic [25:0] req_addr,
    output logic [15:0] req_wdata,
    output logic [1:0] req_bytes,
    input logic ready,
    input logic [15:0] rdata,
    output logic protocol_error
);
    typedef enum logic [2:0] {ARMED, SETTLE, REQUEST, RESPONSE, QUIET, BROKEN} state_t;
    state_t state;
    (* ASYNC_REG = "TRUE" *) logic [2:0] active_sync;
    logic active, legal;
    int unsigned settling;
    assign active = (!ne_ram_n || !ne_control_n) && (!noe_n || !nwe_n);
    assign legal = (ne_ram_n ^ ne_control_n) && (noe_n ^ nwe_n) && nbl_n != 2'b11;
    // Assert wait directly on the bus strobe, before clock-domain capture.
    // The registered read data settles before NWAIT's release is sampled by
    // FSMC. Asynchronously disable DQ when NOE/CS goes inactive.
    assign nwait = !active || (reset_n && state == RESPONSE && !protocol_error);
    assign dq_oe = reset_n && state == RESPONSE && !protocol_error &&
                   active && legal && !noe_n && nwe_n;

    initial if (CAPTURE_CYCLES < 2) $fatal(1, "capture settling must be >=2 clocks");
    always_ff @(posedge clk) begin
        if (!reset_n) begin
            active_sync <= 0;
            state <= ARMED;
            settling <= 0;
            protocol_error <= 0;
            req_valid <= 0;
            req_control <= 0;
            req_write <= 0;
            req_addr <= 0;
            req_wdata <= 0;
            req_bytes <= 0;
            dq_out <= 0;
        end else begin
            active_sync <= {active_sync[1:0], active};
            case (state)
                ARMED: if (active_sync[2]) begin
                    settling <= 0;
                    state <= SETTLE;
                end
                SETTLE: begin
                    if (!active || !active_sync[2] || !legal) begin
                        protocol_error <= 1;
                        state <= BROKEN;
                    end else if (settling == CAPTURE_CYCLES-1) begin
                        req_control <= !ne_control_n;
                        req_write <= !nwe_n;
                        // FSMC A0 is CPU byte-address bit 1 on a 16-bit bus.
                        req_addr <= {address, 1'b0};
                        req_wdata <= dq_in;
                        req_bytes <= ~nbl_n;
                        req_valid <= 1;
                        state <= REQUEST;
                    end else settling <= settling + 1;
                end
                REQUEST: begin
                    // A violating/aborted master cannot cancel a transaction
                    // already accepted by the fabric. Drain it once, freeze
                    // the domain, and never replay it as a fresh bus access.
                    if (!active || !legal || req_control != !ne_control_n ||
                        req_write != !nwe_n || req_addr != {address, 1'b0} ||
                        req_bytes != ~nbl_n || (req_write && req_wdata != dq_in))
                        protocol_error <= 1;
                    if (ready) begin
                        req_valid <= 0;
                        dq_out <= rdata;
                        state <= RESPONSE;
                    end
                end
                RESPONSE: begin
                    if (protocol_error) state <= BROKEN;
                    else if (!active_sync[2]) state <= QUIET;
                end
                // This gap satisfies fabric's valid-withdrawal contract and
                // prevents a held strobe repeating destructive register reads.
                QUIET: state <= ARMED;
                BROKEN: req_valid <= 0;
                default: begin protocol_error <= 1; state <= BROKEN; end
            endcase
        end
    end
endmodule
