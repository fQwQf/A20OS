`timescale 1ns/1ps
// Target-independent glue. Bind dq_oe/dq_out to top-level I/O tri-state cells
// in the selected FPGA wrapper; all MCU ports remain separate electrical buses.
module a20_shared_top #(
    parameter int CPUS = 2,
    parameter int RAM_ADDR_BITS = 20,
    parameter int RAM_RESPONSE_CYCLES = 1
)(
    input logic clk, reset_n,
    input logic [CPUS-1:0] ne_ram_n, ne_control_n, noe_n, nwe_n,
    input logic [CPUS-1:0][24:0] address,
    input logic [CPUS-1:0][1:0] nbl_n,
    input logic [CPUS-1:0][15:0] dq_in,
    output logic [CPUS-1:0][15:0] dq_out,
    output logic [CPUS-1:0] dq_oe, nwait,
    output logic [15:0] fault
);
    logic [CPUS-1:0] req_valid, req_control, req_write, ready, protocol_error;
    logic [CPUS-1:0][25:0] req_addr;
    logic [CPUS-1:0][15:0] req_wdata, rdata;
    logic [CPUS-1:0][1:0] req_bytes;
    logic mem_valid, mem_write, mem_ready;
    logic [RAM_ADDR_BITS-1:0] mem_addr;
    logic [15:0] mem_wdata, mem_rdata, external_fault;
    logic [1:0] mem_bytes;
    assign external_fault = |protocol_error ? 16'h82 : 16'h0;
    for (genvar cpu = 0; cpu < CPUS; cpu++) begin : ports
        a20_fsmc_bridge bridge (
            .clk, .reset_n, .ne_ram_n(ne_ram_n[cpu]), .ne_control_n(ne_control_n[cpu]),
            .noe_n(noe_n[cpu]), .nwe_n(nwe_n[cpu]), .address(address[cpu]),
            .nbl_n(nbl_n[cpu]), .dq_in(dq_in[cpu]), .dq_out(dq_out[cpu]),
            .dq_oe(dq_oe[cpu]), .nwait(nwait[cpu]), .req_valid(req_valid[cpu]),
            .req_control(req_control[cpu]), .req_write(req_write[cpu]),
            .req_addr(req_addr[cpu]), .req_wdata(req_wdata[cpu]),
            .req_bytes(req_bytes[cpu]), .ready(ready[cpu]), .rdata(rdata[cpu]),
            .protocol_error(protocol_error[cpu])
        );
    end
    a20_fabric #(.CPUS(CPUS), .RAM_ADDR_BITS(RAM_ADDR_BITS)) fabric (.*);
    a20_shared_ram #(.RAM_ADDR_BITS(RAM_ADDR_BITS), .RESPONSE_CYCLES(RAM_RESPONSE_CYCLES)) ram (
        .clk, .reset_n, .mem_valid, .mem_write, .mem_addr, .mem_wdata,
        .mem_bytes, .mem_ready, .mem_rdata
    );
endmodule
