`timescale 1ns/1ps
module a20_shared_top_tb;
    localparam int N = 3;
    logic clk = 0;
    always #5 clk = ~clk;
    logic reset_n = 0;
    logic [N-1:0] ne_ram_n = '1, ne_control_n = '1, noe_n = '1, nwe_n = '1;
    logic [N-1:0][24:0] address = '0;
    logic [N-1:0][1:0] nbl_n = '0;
    logic [N-1:0][15:0] dq_in = '0, dq_out;
    logic [N-1:0] dq_oe, nwait;
    logic [15:0] fault;
    int completed = 0;
    // RAM takes 3us, longer than firmware's fixed DATAST without NWAIT.
    a20_shared_top #(.CPUS(N), .RAM_ADDR_BITS(8), .RAM_RESPONSE_CYCLES(300)) dut (.*);

    // Deliberately not aligned to fabric clock edges. The bus master holds
    // payload until NWAIT release, then allows four HCLKs before sampling/
    // ending the strobe (8MHz HCLK here vs 100MHz fabric clock).
    task automatic start_bus(input int cpu, input bit control, input bit wr,
                              input int byte_addr, input logic [15:0] data,
                              input logic [1:0] bytes = 3);
        #3;
        address[cpu] = 25'(byte_addr >> 1);
        dq_in[cpu] = data;
        nbl_n[cpu] = ~bytes;
        ne_ram_n[cpu] = control;
        ne_control_n[cpu] = !control;
        // Address setup precedes NOE/NWE assertion.
        #127;
        noe_n[cpu] = wr;
        nwe_n[cpu] = !wr;
        #1;
        if (nwait[cpu] || dq_oe[cpu]) $fatal(1, "no initial wait / premature DQ");
    endtask

    task automatic finish_bus(input int cpu, input bit wr, output logic [15:0] data);
        int budget = 1000;
        while (!nwait[cpu] && budget > 0) begin #7; budget--; end
        if (!nwait[cpu]) $fatal(1, "bus response timeout cpu=%0d fault=%x", cpu, fault);
        if (dq_oe[cpu] == wr) $fatal(1, "DQ direction mismatch");
        data = dq_out[cpu];
        // Long held strobes must neither repeat side effects nor lose data.
        repeat (50) begin
            #10;
            if (!nwait[cpu] || (!wr && (!dq_oe[cpu] || dq_out[cpu] != data)))
                $fatal(1, "response not held");
        end
        noe_n[cpu] = 1; nwe_n[cpu] = 1;
        #1;
        if (dq_oe[cpu]) $fatal(1, "DQ not released on NOE rising");
        ne_ram_n[cpu] = 1; ne_control_n[cpu] = 1;
        #127;
        completed++;
    endtask

    task automatic transfer(input int cpu, input bit control, input bit wr,
                             input int byte_addr, input logic [15:0] data,
                             output logic [15:0] result, input logic [1:0] bytes = 3);
        start_bus(cpu, control, wr, byte_addr, data, bytes);
        finish_bus(cpu, wr, result);
    endtask

    task automatic expect_reg(input int cpu, input int byte_addr,
                               input logic [15:0] expected);
        logic [15:0] data;
        transfer(cpu, 1, 0, byte_addr, 0, data);
        if (data != expected) $fatal(1, "reg cpu=%0d addr=%x got=%x expected=%x",
                                     cpu, byte_addr, data, expected);
    endtask

    task automatic reset_domain();
        ne_ram_n = '1; ne_control_n = '1; noe_n = '1; nwe_n = '1;
        reset_n = 0;
        repeat (5) @(negedge clk);
        reset_n = 1;
        repeat (5) @(negedge clk);
    endtask

    initial begin
        logic [15:0] data, other;
        reset_domain();
        expect_reg(0, 0, 'h3053);
        expect_reg(0, 2, 'h4132);
        for (int cpu = 0; cpu < N; cpu++) begin
            expect_reg(cpu, 4, 16'(cpu));
            expect_reg(cpu, 8, 16'(N));
            expect_reg(cpu, 'h18, 1);
            expect_reg(cpu, 'h18, 0); // held destructive read executed once
        end
        expect_reg(0, 'h0c, 1);
        expect_reg(0, 'h0c, 0);
        transfer(0, 1, 1, 'h10, 6, data);
        expect_reg(1, 'h10, 6);
        transfer(0, 1, 1, 'h20, 6, data);
        expect_reg(1, 'h24, 1);
        expect_reg(1, 'h24, 0);
        // Concurrent independent physical buses contend in the same fabric.
        #3;
        address[0] = 'h80; address[1] = 'h80;
        ne_control_n[0] = 0; ne_control_n[1] = 0;
        nbl_n[0] = 0; nbl_n[1] = 0;
        #127; noe_n[0] = 0; noe_n[1] = 0;
        #1;
        if (nwait[0] || nwait[1]) $fatal(1, "concurrent bus initial wait");
        finish_bus(0, 0, data);
        finish_bus(1, 0, other);
        if (int'(data) + int'(other) != 1) $fatal(1, "concurrent gate ownership");
        transfer(data == 1 ? 0 : 1, 1, 1, 'h100, 1, data);
        transfer(0, 0, 1, 4, 'h1234, data);
        transfer(1, 0, 1, 4, 'h00aa, data, 1);
        transfer(2, 0, 1, 4, 'hbb00, data, 2);
        transfer(1, 0, 0, 4, 0, data);
        if (data != 'hbbaa) $fatal(1, "cross-port RAM/byte lanes");
        // Alternating neighboring halfwords represent a 32-bit CPU access.
        #3;
        address[0] = 3; address[2] = 5;
        dq_in[0] = 'ha20f; dq_in[2] = 'habcd;
        nbl_n[0] = 0; nbl_n[2] = 0;
        ne_ram_n[0] = 0; ne_ram_n[2] = 0;
        #127; nwe_n[0] = 0; nwe_n[2] = 0;
        #1;
        if (nwait[0] || nwait[2]) $fatal(1, "concurrent RAM initial wait");
        finish_bus(0, 1, data);
        finish_bus(2, 1, other);
        transfer(0, 0, 1, 8, 'h5678, data);
        transfer(2, 0, 1, 12, 'hfedc, other);
        transfer(1, 0, 0, 6, 0, data);
        if (data != 'ha20f) $fatal(1, "concurrent RAM address");
        transfer(1, 0, 0, 8, 0, data);
        if (data != 'h5678) $fatal(1, "second halfword");
        // Whole-domain reset clears controller state but retains RAM bytes.
        reset_domain();
        transfer(2, 0, 0, 4, 0, data);
        if (data != 'hbbaa) $fatal(1, "RAM reset destroyed contents");
        expect_reg(0, 'h0c, 1);
        expect_reg(1, 'h18, 1);
        // An aborted bus request drains once and freezes the domain.
        start_bus(0, 0, 1, 4, 'hffff);
        wait (dut.req_valid[0]);
        #1; nwe_n[0] = 1; ne_ram_n[0] = 1;
        #4000;
        if (fault != 'h82 || dq_oe[0]) $fatal(1, "abort did not latch bridge fault");
        expect_reg(1, 'h14, 'h82); // healthy port still reads global fault
        if (dut.mem_valid) $fatal(1, "aborted transaction not drained");
        reset_domain();
        expect_reg(0, 'h14, 0);
        // Malformed dual chip-selects never issue a RAM operation.
        start_bus(0, 0, 0, 0, 0);
        ne_control_n[0] = 0;
        #300;
        if (fault != 'h82 || dq_oe[0] || nwait[0]) $fatal(1, "illegal select accepted");
        expect_reg(2, 'h14, 'h82);
        $display("a20_shared_top_tb: PASS asynchronous buses, wait, DQ, one-shot registers, concurrent gates/RAM, byte lanes, reset, abort fence (%0d transactions)", completed);
        $finish;
    end
    initial begin #200000; $fatal(1, "testbench watchdog"); end
endmodule
