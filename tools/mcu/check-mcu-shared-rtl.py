#!/usr/bin/env python3
"""Execute native MCU firmware with shared accesses supplied by real RTL.

Unicorn executes instructions/private peripherals. Every shared-memory and
fabric access goes through Verilator's FSMC pin adapter and RTL-source
bridge/arbiter/RAM. Sequential bus RPC is functional integration, not timing or
physical hardware evidence. The separate bus test covers concurrent requests.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess

spec = importlib.util.spec_from_file_location(
    "shared_kernel_test", Path(__file__).with_name("check-mcu-shared-kernel.py"))
kernel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kernel)


class Bus:
    def __init__(self, executable):
        self.process = subprocess.Popen([str(executable.resolve())], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, bufsize=1)
        self.accesses = [0, 0]
        self.shared_reads = self.shared_writes = 0
        assert self.command("h") == "2 1048576", "RTL harness topology/capacity mismatch"

    def command(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        response = self.process.stdout.readline().strip()
        if not response:
            raise RuntimeError(f"RTL adapter stopped: {text}")
        return response

    def access(self, cpu, control, write, address, size, value=0):
        self.accesses[cpu] += 1
        if not control:
            if write:
                self.shared_writes += 1
            else:
                self.shared_reads += 1
        return int(self.command(f"x {cpu} {int(control)} {int(write)} {address} {size} {value & ((1 << (size * 8)) - 1)}"))

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.write("q\n")
            self.process.stdin.flush()
            try:
                assert self.process.wait(timeout=5) == 0
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise


class RtlDomain(kernel.Domain):
    def __init__(self, elf, bus, seed, quantum):
        self.bus = bus
        super().__init__(elf, 2, seed, quantum)
        self.wait_verified = [False, False]
        assert self.shared_size == 1048576
        from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
        for cpu, engine in enumerate(self.engines):
            engine.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.shared_access,
                            cpu, kernel.SHARED, kernel.SHARED + self.shared_size - 1)

    def shared_access(self, uc, access, address, size, value, cpu):
        from unicorn import UC_MEM_WRITE
        if access == UC_MEM_WRITE:
            self.bus.access(cpu, False, True, address - kernel.SHARED, size, value)
        else:
            result = self.bus.access(cpu, False, False, address - kernel.SHARED, size)
            # Refresh the instruction engine's mirror from RTL before its
            # load; the mirror supplies no read values independently.
            offset = address - kernel.SHARED
            self.shared[offset:offset + size] = result.to_bytes(size, "little")

    def fabric_read(self, uc, offset, size, cpu):
        if not self.wait_verified[cpu]:
            for reg in (0xa0000000, 0xa0000008):
                assert int.from_bytes(uc.mem_read(reg, 4), "little") == 0x9011
            assert (self.peripherals[cpu][0x11400] >> 24) & 15 == 4, "NWAIT must be input"
            self.wait_verified[cpu] = True
        expected = super().fabric_read(uc, offset, size, cpu)
        actual = self.bus.access(cpu, True, False, offset, size)
        assert actual == expected, ("RTL register differs", cpu, offset, actual, expected)
        return actual

    def fabric_write(self, uc, offset, size, value, cpu):
        super().fabric_write(uc, offset, size, value, cpu)
        self.bus.access(cpu, True, True, offset, size, value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--backend", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20)
    # Unicorn 2.1.4 skips a Thumb instruction at some multi-instruction
    # boundaries when RAM hooks are installed. Even a no-op hook reproduces
    # it. Single-instruction stepping preserves the real instruction stream.
    parser.add_argument("--quantum", type=int, choices=[1], default=1)
    parser.add_argument("--rounds", type=int, default=5000000)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--objdump", default="arm-none-eabi-objdump")
    args = parser.parse_args()
    if args.quantum < 1 or args.rounds < 1:
        parser.error("quantum and rounds must be positive")
    disassembly = subprocess.check_output([args.objdump, "-d", str(args.elf)], text=True)
    assert not re.search(r"\b(?:ldrex|strex)(?:b|h|d)?\b", disassembly, re.I)
    bus = Bus(args.backend)
    try:
        domain = RtlDomain(args.elf, bus, args.seed, args.quantum)
        result = domain.run(args.rounds, reset_cpus=[1])
        assert int(bus.command("f")) == 5, "RTL did not latch isolated CPU1 reset fault"
        result.update(single_cpu_reset="CPU1 rejected, shared memory unchanged; RTL fault=5",
                      evidence="instruction emulation with real FSMC bridge/fabric/RAM RTL; not physical hardware",
                      rtl_bus_accesses=bus.accesses, rtl_shared_reads=bus.shared_reads,
                      rtl_shared_writes=bus.shared_writes, seed=args.seed, quantum=args.quantum)
        result["firmware_async_wait_verified"] = domain.wait_verified
        assert min(bus.accesses) > 0 and bus.shared_reads > 0 and bus.shared_writes > 0
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    finally:
        bus.close()


if __name__ == "__main__":
    main()
