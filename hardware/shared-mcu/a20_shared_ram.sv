`timescale 1ns/1ps
// Inferred synchronous FPGA RAM backend, byte enables, one operation per
// valid/ready handshake. RAM contents are intentionally not reset/cleared.
// Capacity must actually fit the selected FPGA; no external SDRAM implied.
module a20_shared_ram #(
    parameter int RAM_ADDR_BITS = 20,
    parameter int RESPONSE_CYCLES = 1
)(
    input logic clk, reset_n,
    input logic mem_valid, mem_write,
    input logic [RAM_ADDR_BITS-1:0] mem_addr,
    input logic [15:0] mem_wdata,
    input logic [1:0] mem_bytes,
    output logic mem_ready,
    output logic [15:0] mem_rdata
);
    localparam int WORDS = 1 << (RAM_ADDR_BITS-1);
    logic [7:0] low_byte[WORDS], high_byte[WORDS];
    int unsigned waiting;
    initial if (RAM_ADDR_BITS < 2 || RAM_ADDR_BITS > 26)
        $fatal(1, "invalid RAM address width");
    initial if (RESPONSE_CYCLES < 1) $fatal(1, "invalid RAM response latency");
    always_ff @(posedge clk) begin
        if (!reset_n) begin
            mem_ready <= 0;
            mem_rdata <= 0;
            waiting <= 0;
        end else if (!mem_valid) begin
            mem_ready <= 0;
            waiting <= 0;
        end else if (!mem_ready) begin
            if (waiting == RESPONSE_CYCLES-1) begin
                assert (!mem_addr[0]) else $fatal(1, "RAM requires aligned halfwords");
                if (mem_write) begin
                    if (mem_bytes[0]) low_byte[mem_addr[RAM_ADDR_BITS-1:1]] <= mem_wdata[7:0];
                    if (mem_bytes[1]) high_byte[mem_addr[RAM_ADDR_BITS-1:1]] <= mem_wdata[15:8];
                end
                mem_rdata <= {high_byte[mem_addr[RAM_ADDR_BITS-1:1]],
                             low_byte[mem_addr[RAM_ADDR_BITS-1:1]]};
                mem_ready <= 1;
            end else waiting <= waiting + 1;
        end
    end
endmodule
