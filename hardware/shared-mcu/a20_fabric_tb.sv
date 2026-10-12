`timescale 1ns/1ps
module a20_fabric_tb;
    localparam int N = 3;
    logic clk = 0;
    always #5 clk = ~clk;
    logic reset_n = 0;
    logic [15:0] external_fault = 0;
    logic [N-1:0] req_valid = 0, req_control = 0, req_write = 0;
    logic [N-1:0][25:0] req_addr = 0;
    logic [N-1:0][15:0] req_wdata = 0;
    logic [N-1:0][1:0] req_bytes = '1;
    logic [N-1:0] ready;
    logic [N-1:0][15:0] rdata;
    logic mem_valid, mem_write, mem_ready;
    logic [19:0] mem_addr;
    logic [15:0] mem_wdata, mem_rdata, fault;
    logic [1:0] mem_bytes;
    logic [15:0] ram[64];
    int latency = 0;
    int accesses = 0;
    a20_fabric #(.CPUS(N), .RAM_ADDR_BITS(20)) dut (.*);

    // Deliberately delayed memory completion; grant/release must not pass it.
    always_ff @(posedge clk) begin
        mem_ready <= 0;
        if (!reset_n) begin
            latency <= 0;
            accesses <= 0;
            mem_rdata <= 0;
        end else if (mem_valid && !mem_ready) begin
            if (latency == 2) begin
                if (mem_addr >= 128) $fatal(1, "test RAM bounds");
                if (mem_write) begin
                    if (mem_bytes[0]) ram[mem_addr[6:1]][7:0] <= mem_wdata[7:0];
                    if (mem_bytes[1]) ram[mem_addr[6:1]][15:8] <= mem_wdata[15:8];
                end
                mem_rdata <= ram[mem_addr[6:1]];
                mem_ready <= 1;
                accesses <= accesses + 1;
                latency <= 0;
            end else latency <= latency + 1;
        end
    end

    task automatic reset_domain();
        @(negedge clk); reset_n = 0; req_valid = 0;
        repeat (3) @(negedge clk);
        reset_n = 1;
        @(negedge clk);
    endtask

    task automatic request(input int cpu, input bit control,
                           input bit write_op, input int address,
                           input logic [15:0] value);
        @(negedge clk);
        req_control[cpu] = control;
        req_write[cpu] = write_op;
        req_addr[cpu] = 26'(address);
        req_wdata[cpu] = value;
        req_valid[cpu] = 1;
    endtask

    task automatic response(input int cpu, output logic [15:0] value);
        int budget;
        budget = 100;
        while (!ready[cpu] && budget > 0) begin
            @(negedge clk); budget--;
        end
        if (!ready[cpu]) $fatal(1, "arbiter starved port %0d", cpu);
        value = rdata[cpu];
        req_valid[cpu] = 0;
        @(negedge clk);
    endtask

    task automatic read_expect(input int cpu, input int address,
                               input logic [15:0] expected);
        logic [15:0] value;
        request(cpu, 1, 0, address, 0);
        response(cpu, value);
        if (value != expected)
            $fatal(1, "read cpu=%0d addr=%x got=%x expected=%x", cpu,
                   address, value, expected);
    endtask

    task automatic write_reg(input int cpu, input int address,
                             input logic [15:0] value);
        logic [15:0] ignored;
        request(cpu, 1, 1, address, value);
        response(cpu, ignored);
    endtask

    initial begin
        logic [15:0] value;
        int winners;
        for (int i = 0; i < 64; i++) ram[i] = 0;
        reset_domain();
        read_expect(0, 'h00, 'h3053);
        read_expect(0, 'h02, 'h4132);
        for (int cpu = 0; cpu < N; cpu++) begin
            read_expect(cpu, 'h04, 16'(cpu));
            read_expect(cpu, 'h08, 16'(N));
            read_expect(cpu, 'h18, 1);
            read_expect(cpu, 'h18, 0);
        end
        read_expect(1, 'h0c, 0);
        read_expect(0, 'h0c, 1);
        read_expect(0, 'h0c, 0);
        write_reg(0, 'h10, 2);
        write_reg(0, 'h10, 4);
        read_expect(1, 'h10, 6);
        write_reg(0, 'h20, 6);
        read_expect(1, 'h24, 1);
        read_expect(1, 'h24, 0);
        read_expect(2, 'h24, 1);
        // Every waiting port gets a response; exactly one wins the gate.
        @(negedge clk);
        req_valid = '1; req_control = '1; req_write = '0;
        for (int i = 0; i < N; i++) req_addr[i] = 'h100;
        winners = 0;
        for (int i = 0; i < N; i++) begin
            response(i, value);
            winners += int'(value);
        end
        if (winners != 1 || !dut.lock_valid[0]) $fatal(1, "multiple gate owners");
        write_reg(int'(dut.lock_owner[0]), 'h100, 1);
        read_expect(2, 'h104, 1); // independent heap gate
        read_expect(0, 'h100, 1);
        request(0, 0, 1, 4, 'ha20f);
        // Queue control traffic behind an incomplete shared-memory write.
        while (!mem_valid) @(negedge clk);
        request(2, 1, 0, 'h00, 0);
        while (!ready[0]) begin
            if (ready[2]) $fatal(1, "control passed incomplete RAM write");
            @(negedge clk);
        end
        response(0, value);
        response(2, value);
        if (value != 'h3053) $fatal(1, "queued control read failed");
        write_reg(0, 'h100, 1);
        read_expect(1, 'h100, 1);
        request(1, 0, 0, 4, 0);
        response(1, value);
        if (value != 'ha20f || accesses != 2) $fatal(1, "RAM publication failed");
        write_reg(1, 'h100, 1);
        write_reg(2, 'h104, 1);
        // Byte lanes support compiler-emitted byte accesses on a 16-bit bus.
        req_bytes[0] = 1;
        request(0, 0, 1, 4, 'h00aa);
        response(0, value);
        req_bytes[0] = 2;
        request(0, 0, 1, 4, 'hbb00);
        response(0, value);
        req_bytes[0] = 3;
        request(1, 0, 0, 4, 0);
        response(1, value);
        if (value != 'hbbaa || accesses != 5) $fatal(1, "byte lanes failed");
        // Wrong-owner release is fatal and freezes subsequent RAM traffic.
        read_expect(0, 'h100, 1);
        write_reg(1, 'h100, 1);
        read_expect(2, 'h14, 'h80);
        request(2, 0, 1, 4, 'hffff);
        response(2, value);
        if (ram[2] != 'hbbaa || accesses != 5) $fatal(1, "fault did not fence RAM");
        write_reg(0, 'h14, 0);
        read_expect(0, 'h14, 'h80);
        // Only a domain reset releases boot/attach/lock/fault ownership.
        reset_domain();
        read_expect(0, 'h14, 0);
        read_expect(0, 'h0c, 1);
        read_expect(1, 'h18, 1);
        read_expect(1, 'h100, 1);
        req_bytes[2] = 1;
        request(2, 1, 0, 'h04, 0);
        response(2, value);
        req_bytes[2] = 3;
        read_expect(0, 'h14, 'h81);
        $display("a20_fabric_tb: PASS topology, boot, attach, release, IPI, gates, RAM, fault fence, reset");
        $finish;
    end
    initial begin
        #100000;
        $fatal(1, "testbench watchdog");
    end
endmodule
