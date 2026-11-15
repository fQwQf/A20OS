// Test adapter: firmware accesses enter the actual RTL FSMC pins, bridge,
// arbiter and inferred RAM. It does not implement fabric register semantics.
#include "Va20_shared_top.h"
#include "verilated.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

static Va20_shared_top top;
static void cycle(unsigned n = 1) {
    while (n--) {
        top.clk = 0;
        top.eval();
        Verilated::timeInc(5);
        top.clk = 1;
        top.eval();
        Verilated::timeInc(5);
    }
}
static void bit(uint8_t &field, unsigned cpu, bool value) {
    field = (field & ~(1U << cpu)) | (unsigned(value) << cpu);
}
static uint16_t halfword(unsigned cpu, bool control, bool write,
                         unsigned address, uint16_t value, unsigned lanes) {
    const unsigned addr_shift = cpu * 25, data_shift = cpu * 16;
    top.address = (top.address & ~(uint64_t(0x1ffffff) << addr_shift)) |
                  (uint64_t(address >> 1) << addr_shift);
    top.dq_in = (top.dq_in & ~(uint32_t(0xffff) << data_shift)) |
                (uint32_t(value) << data_shift);
    top.nbl_n = (top.nbl_n & ~(3U << (cpu * 2))) | ((~lanes & 3) << (cpu * 2));
    bit(top.ne_ram_n, cpu, control);
    bit(top.ne_control_n, cpu, !control);
    cycle(16); // address setup, stable bundled payload
    bit(top.noe_n, cpu, write);
    bit(top.nwe_n, cpu, !write);
    top.eval();
    if ((top.nwait | top.dq_oe) & (1U << cpu))
        throw std::runtime_error("missing initial wait or premature DQ drive");
    unsigned budget = 10000;
    while (!(top.nwait & (1U << cpu)) && budget--)
        cycle();
    if (!(top.nwait & (1U << cpu)))
        throw std::runtime_error("RTL bus timeout");
    uint16_t result = top.dq_out >> data_shift;
    cycle(50); // four 8MHz HCLKs after wait release
    if (bool(top.dq_oe & (1U << cpu)) == write ||
        (!write && uint16_t(top.dq_out >> data_shift) != result))
        throw std::runtime_error("RTL DQ direction/data hold failure");
    bit(top.noe_n, cpu, true);
    bit(top.nwe_n, cpu, true);
    top.eval();
    if (top.dq_oe & (1U << cpu))
        throw std::runtime_error("RTL DQ not released");
    bit(top.ne_ram_n, cpu, true);
    bit(top.ne_control_n, cpu, true);
    cycle(16);
    return result;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    // This harness is deliberately compiled for two physical ports only.
    top.ne_ram_n = top.ne_control_n = top.noe_n = top.nwe_n = 3;
    top.reset_n = 0;
    cycle(5);
    top.reset_n = 1;
    cycle(5);
    std::string command;
    try {
        while (std::cin >> command) {
            if (command == "q")
                break;
            if (command == "h") {
                std::cout << "2 1048576\n" << std::flush;
                continue;
            }
            if (command == "f") {
                std::cout << unsigned(top.fault) << '\n' << std::flush;
                continue;
            }
            unsigned cpu, control, write, address, size;
            uint64_t value;
            if (command != "x" ||
                !(std::cin >> cpu >> control >> write >> address >> size >>
                  value) ||
                cpu >= 2 || control > 1 || write > 1 ||
                (size != 1 && size != 2 && size != 4 && size != 8) ||
                (control && size != 1 && (address & 1)) ||
                address >= (1U << 26) || size > (1U << 26) - address)
                throw std::runtime_error("invalid cosimulation request");
            uint64_t result = 0;
            // Cortex-M3 normal-memory unaligned accesses are split into bus
            // beats. FSMC exposes aligned 16-bit words plus byte lanes; never
            // pass a fictitious odd halfword transaction to the RTL pins.
            for (unsigned offset = 0; offset < size;) {
                unsigned current = address + offset;
                unsigned width = !(current & 1) && size - offset >= 2 ? 2 : 1;
                unsigned shift = (current & 1) * 8;
                unsigned lanes = width == 2 ? 3 : 1U << (current & 1);
                unsigned mask = width == 2 ? 65535 : 255;
                uint16_t beat = halfword(cpu, control, write, current & ~1U,
                    uint16_t((value >> (offset * 8)) << shift), lanes);
                result |= uint64_t((beat >> shift) & mask) << (offset * 8);
                offset += width;
            }
            std::cout << result << '\n' << std::flush;
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    top.final();
    return 0;
}
