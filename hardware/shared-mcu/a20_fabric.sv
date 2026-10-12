`timescale 1ns/1ps
// A20 fabric v1 transaction core. Synchronous 16-bit ports, one outstanding
// request per port. A physical FSMC bridge and RAM controller are external.
// Request must stay stable until ready; withdraw valid for >=1 clock after
// ready before submitting another request on that port.
module a20_fabric #(
    parameter int CPUS = 2,
    parameter int RAM_ADDR_BITS = 20
)(
    input logic clk,
    input logic reset_n,
    input logic [CPUS-1:0] req_valid,
    input logic [CPUS-1:0] req_control,
    input logic [CPUS-1:0] req_write,
    input logic [CPUS-1:0][25:0] req_addr,
    input logic [CPUS-1:0][15:0] req_wdata,
    input logic [CPUS-1:0][1:0] req_bytes,
    output logic [CPUS-1:0] ready,
    output logic [CPUS-1:0][15:0] rdata,
    output logic mem_valid,
    output logic mem_write,
    output logic [RAM_ADDR_BITS-1:0] mem_addr,
    output logic [15:0] mem_wdata,
    output logic [1:0] mem_bytes,
    input logic mem_ready,
    input logic [15:0] mem_rdata,
    output logic [15:0] fault
);
    logic claimed;
    logic [CPUS-1:0] attached, released, ipi, consumed;
    logic [1:0] lock_valid;
    int unsigned lock_owner[2];
    int unsigned cursor, selected, memory_owner;
    logic found, memory_busy;

    initial begin
        if (CPUS < 2 || CPUS > 16) $fatal(1, "CPUS must be 2..16");
        if (RAM_ADDR_BITS < 2 || RAM_ADDR_BITS > 26)
            $fatal(1, "invalid RAM address width");
    end

    always_comb begin
        selected = 0;
        found = 0;
        for (int step = 0; step < CPUS; step++) begin
            int index;
            index = int'(cursor) + step;
            if (index >= CPUS) index -= CPUS;
            if (!found && req_valid[index] && !consumed[index]) begin
                selected = unsigned'(index);
                found = 1;
            end
        end
    end

    // Control operations and RAM requests share one serialization point.
    // A lock grant/release cannot pass an incomplete RAM write on any port.
    always_ff @(posedge clk) begin
        if (!reset_n) begin
            claimed <= 0;
            attached <= '0;
            released <= '0;
            ipi <= '0;
            consumed <= '0;
            lock_valid <= '0;
            lock_owner[0] <= 0;
            lock_owner[1] <= 0;
            cursor <= 0;
            memory_owner <= 0;
            memory_busy <= 0;
            mem_valid <= 0;
            mem_write <= 0;
            mem_addr <= '0;
            mem_wdata <= 0;
            mem_bytes <= 0;
            ready <= '0;
            rdata <= '0;
            fault <= 0;
        end else begin
            ready <= '0;
            for (int i = 0; i < CPUS; i++)
                if (!req_valid[i]) consumed[i] <= 0;
            if (memory_busy) begin
                if (mem_ready) begin
                    ready[memory_owner] <= 1;
                    rdata[memory_owner] <= mem_rdata;
                    consumed[memory_owner] <= 1;
                    cursor <= memory_owner == CPUS-1 ? 0 : memory_owner + 1;
                    memory_busy <= 0;
                    mem_valid <= 0;
                end
            end else if (found) begin
                if (!req_control[selected] && fault == 0 &&
                    req_addr[selected][0] == 0 && req_bytes[selected] != 0 &&
                    (req_addr[selected] >> RAM_ADDR_BITS) == 0) begin
                    memory_owner <= selected;
                    mem_write <= req_write[selected];
                    mem_addr <= req_addr[selected][RAM_ADDR_BITS-1:0];
                    mem_wdata <= req_wdata[selected];
                    mem_bytes <= req_bytes[selected];
                    mem_valid <= 1;
                    memory_busy <= 1;
                end else begin
                    ready[selected] <= 1;
                    rdata[selected] <= 0;
                    consumed[selected] <= 1;
                    cursor <= selected == CPUS-1 ? 0 : selected + 1;
                    if (fault != 0) begin
                        // Frozen domain: no further RAM transaction or lock
                        // grant can mutate state. FAULT remains readable.
                        if (req_control[selected] && !req_write[selected] &&
                            req_addr[selected] == 26'h14)
                            rdata[selected] <= fault;
                    end else if (!req_control[selected] ||
                                 req_bytes[selected] != 2'b11 ||
                                 req_addr[selected][0] != 0) begin
                        fault <= 16'h81;
                    end else if (req_write[selected]) begin
                        case (req_addr[selected])
                            26'h10: begin
                                if (selected == 0 && claimed)
                                    released <= released | req_wdata[selected][CPUS-1:0];
                                else fault <= 16'h81;
                            end
                            26'h14: begin
                                if (req_wdata[selected] != 0)
                                    fault <= req_wdata[selected];
                            end
                            26'h20: ipi <= ipi | req_wdata[selected][CPUS-1:0];
                            26'h100, 26'h104: begin
                                if (lock_valid[req_addr[selected][2]] &&
                                    lock_owner[req_addr[selected][2]] == selected &&
                                    req_wdata[selected] == 1)
                                    lock_valid[req_addr[selected][2]] <= 0;
                                else fault <= 16'h80;
                            end
                            default: fault <= 16'h81;
                        endcase
                    end else begin
                        case (req_addr[selected])
                            26'h00: rdata[selected] <= 16'h3053;
                            26'h02: rdata[selected] <= 16'h4132;
                            26'h04: rdata[selected] <= 16'(selected);
                            26'h08: rdata[selected] <= 16'(CPUS);
                            26'h0c: begin
                                if (selected == 0 && !claimed) begin
                                    claimed <= 1;
                                    rdata[selected] <= 1;
                                end
                            end
                            26'h10: rdata[selected] <= 16'(released);
                            26'h14: rdata[selected] <= 0;
                            26'h18: begin
                                rdata[selected] <= {15'b0, !attached[selected]};
                                attached[selected] <= 1;
                            end
                            26'h24: begin
                                rdata[selected] <= {15'b0, ipi[selected]};
                                ipi[selected] <= 0;
                            end
                            26'h100, 26'h104: begin
                                if (!lock_valid[req_addr[selected][2]]) begin
                                    lock_valid[req_addr[selected][2]] <= 1;
                                    lock_owner[req_addr[selected][2]] <= selected;
                                    rdata[selected] <= 1;
                                end
                            end
                            default: fault <= 16'h81;
                        endcase
                    end
                end
            end
        end
    end
endmodule
