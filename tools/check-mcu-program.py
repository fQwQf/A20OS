#!/usr/bin/env python3
"""Execute native ARM PIE applications, including real SVC handler instructions.

The processor model implements architectural exception stacking/unstacking;
kernel loader, SVC dispatch, user instructions and scheduling run firmware.
Optional --backend routes shared data AND instruction-byte checks through RTL.
No task state is modified by the host. Not physical board evidence.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


rtl = module("shared_rtl", "check-mcu-shared-rtl.py")
kernel = rtl.kernel
from unicorn import UC_HOOK_CODE, UC_HOOK_INTR, UcError, UC_ERR_INSN_INVALID
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_R3, UC_ARM_REG_R7, UC_ARM_REG_R12, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_SP, UC_ARM_REG_PSP, UC_ARM_REG_MSP, UC_ARM_REG_XPSR,
    UC_ARM_REG_CONTROL, UC_ARM_REG_PRIMASK)
VOLATILE = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
            UC_ARM_REG_R12, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_XPSR]


class ProgramDomain(rtl.RtlDomain):
    def __init__(self, elf, bus, cpus, seed):
        self.bus = bus
        if bus:
            super().__init__(elf, bus, seed, 1)
        else:
            kernel.Domain.__init__(self, elf, cpus, seed, 1)
        assert self.program_mode, "requires MCU_NATIVE_PROGRAM=1 selftest firmware"
        self.exceptions = [0] * self.n
        self.user_instructions = [0] * self.n
        self.syscall_numbers = set()
        self.svc_count = [0] * self.n
        self.returns = [0] * self.n
        self.private_stack_checks = [0] * self.n
        # Read the production ELF allocation size; the test does not allocate
        # or switch firmware stacks on the kernel's behalf.
        with open(elf, "rb") as stream:
            obj = kernel.ELFFile(stream)
            stack_size = next(int(sym["st_size"]) for sym in
                              obj.get_section_by_name(".symtab").iter_symbols()
                              if sym.name == "armv7m_exception_stack")
        self.exception_stack_base = self.symbols["armv7m_exception_stack"]
        self.exception_stack_top = self.exception_stack_base + stack_size
        assert stack_size >= 32 and kernel.LOCAL <= self.exception_stack_base
        assert self.exception_stack_top <= self.symbols["_stack_limit"]
        self.return_addresses = set()
        for first, end in [("SVC_Handler", "PendSV_Handler"),
                           ("SysTick_Handler", "External_IRQHandler")]:
            start = self.symbols[first] & ~1
            stop = self.symbols[end] & ~1
            for address in range(start, stop, 2):
                if bytes(self.engines[0].mem_read(address, 2)) == b"\x70\x47":
                    self.return_addresses.add(address)
        assert len(self.return_addresses) == 2
        for cpu, engine in enumerate(self.engines):
            engine.hook_add(UC_HOOK_INTR, self.interrupt, cpu)
            engine.hook_add(UC_HOOK_CODE, self.instruction, cpu)

    def fabric_read(self, uc, offset, size, cpu):
        if self.bus:
            return super().fabric_read(uc, offset, size, cpu)
        return kernel.Domain.fabric_read(self, uc, offset, size, cpu)

    def fabric_write(self, uc, offset, size, value, cpu):
        if self.bus:
            return super().fabric_write(uc, offset, size, value, cpu)
        return kernel.Domain.fabric_write(self, uc, offset, size, value, cpu)

    def read_hardware(self, cpu, address, size):
        if self.bus and kernel.SHARED <= address < kernel.SHARED + self.shared_size:
            result = bytearray()
            for off in range(0, size, 2):
                value = self.bus.access(cpu, False, False, address-kernel.SHARED+off, 2)
                result.extend(value.to_bytes(2, "little"))
            return bytes(result)
        return bytes(self.engines[cpu].mem_read(address, size))

    def write_hardware(self, cpu, address, data):
        if self.bus and kernel.SHARED <= address < kernel.SHARED + self.shared_size:
            for off in range(0, len(data), 2):
                self.bus.access(cpu, False, True, address-kernel.SHARED+off, 2,
                                int.from_bytes(data[off:off+2], "little"))
        self.engines[cpu].mem_write(address, data)

    def exception_enter(self, cpu, number):
        engine = self.engines[cpu]
        assert not self.exceptions[cpu], "nested exceptions outside test model"
        old_sp = engine.reg_read(UC_ARM_REG_SP)
        use_psp = bool(engine.reg_read(UC_ARM_REG_CONTROL) & 2)
        values = [engine.reg_read(reg) for reg in VOLATILE]
        values[7] = (values[7] & ~0x200) | (0x200 if old_sp & 4 else 0)
        frame = (old_sp - 32) & ~7
        self.write_hardware(cpu, frame, b"".join(v.to_bytes(4, "little") for v in values))
        # Enter Handler mode first so banked-register API access is privileged.
        psr = values[7] & ~((0x3f << 10) | (3 << 25) | 0x1ff)
        engine.reg_write(UC_ARM_REG_XPSR, psr | number | 0x01000000)
        # Unprivileged Thread mode cannot read banked MSP through the CPU
        # API; inspect it only after entering privileged Handler mode.
        if use_psp:
            msp = engine.reg_read(UC_ARM_REG_MSP)
            assert self.exception_stack_base + 32 <= msp <= self.exception_stack_top, (
                "PSP task left another migratable task stack as handler MSP", cpu, hex(msp))
            self.private_stack_checks[cpu] += 1
        engine.reg_write(UC_ARM_REG_PSP if use_psp else UC_ARM_REG_MSP, frame)
        engine.reg_write(UC_ARM_REG_LR, 0xfffffffd if use_psp else 0xfffffff9)
        engine.reg_write(UC_ARM_REG_PC, self.symbols["SVC_Handler" if number == 11 else "SysTick_Handler"])
        self.exceptions[cpu] = number

    def exception_return(self, cpu, token):
        engine = self.engines[cpu]
        assert token in (0xfffffff9, 0xfffffffd) and self.exceptions[cpu]
        bank = UC_ARM_REG_PSP if token & 4 else UC_ARM_REG_MSP
        frame = engine.reg_read(bank)
        data = self.read_hardware(cpu, frame, 32)
        values = [int.from_bytes(data[o:o+4], "little") for o in range(0, 32, 4)]
        assert values[7] & 0x01000000 and not values[7] & 0x1ff
        engine.reg_write(bank, frame + 32 + (4 if values[7] & 0x200 else 0))
        for reg, value in zip(VOLATILE, values):
            engine.reg_write(reg, value)
        self.exceptions[cpu] = 0
        self.returns[cpu] += 1

    def interrupt(self, engine, number, cpu):
        assert number == 2, ("unexpected instruction exception", number)
        pc = engine.reg_read(UC_ARM_REG_PC)
        opcode = self.read_hardware(cpu, pc-2, 2)
        assert opcode[1] == 0xdf
        if kernel.SHARED <= pc < kernel.SHARED + self.shared_size:
            assert engine.reg_read(UC_ARM_REG_CONTROL) & 3 == 3
            self.syscall_numbers.add(engine.reg_read(UC_ARM_REG_R7))
        self.svc_count[cpu] += 1
        self.exception_enter(cpu, 11)
        engine.emu_stop()

    def instruction(self, engine, address, size, cpu):
        if address in self.return_addresses:
            token = engine.reg_read(UC_ARM_REG_LR)
            if token >= 0xfffffff0:
                self.exception_return(cpu, token)
                engine.emu_stop()  # model CPU EXC_RETURN, not a kernel shortcut
        elif kernel.SHARED <= address < kernel.SHARED + self.shared_size:
            assert engine.reg_read(UC_ARM_REG_CONTROL) & 3 == 3
            assert engine.reg_read(UC_ARM_REG_XPSR) & 0x1ff == 0
            assert self.read_hardware(cpu, address, size) == bytes(engine.mem_read(address, size))
            self.user_instructions[cpu] += 1

    def step(self, cpu, inject_tick):
        engine = self.engines[cpu]
        if engine.reg_read(UC_ARM_REG_PC) == self.symbols["Reset_Handler"] & ~1:
            engine.reg_write(UC_ARM_REG_CONTROL, 0)
            engine.reg_write(UC_ARM_REG_XPSR, 0x01000000)
            self.exceptions[cpu] = 0
        csr = int.from_bytes(engine.mem_read(0xe000e010, 4), "little")
        if inject_tick and not self.exceptions[cpu] and csr & 3 == 3 and not engine.reg_read(UC_ARM_REG_PRIMASK):
            self.exception_enter(cpu, 15)
            self.ticks[cpu] += 1
        self.instructions[cpu] += 1
        pc = engine.reg_read(UC_ARM_REG_PC)
        try:
            engine.emu_start(pc | 1, 0, count=1)
        except UcError as error:
            current = engine.reg_read(UC_ARM_REG_PC)
            if error.errno == UC_ERR_INSN_INVALID and bytes(engine.mem_read(current-2, 2)) == b"\x10\xbf":
                return
            raise RuntimeError(f"cpu={cpu} pc={current:#x} sp={engine.reg_read(UC_ARM_REG_SP):#x} exception={self.exceptions[cpu]}") from error
        assert all("[PANIC]" not in log and "fault exception=" not in log for log in self.logs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--backend", type=Path)
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--seed", type=int, default=20)
    parser.add_argument("--rounds", type=int, default=5000000)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if not 2 <= args.cpus <= 16 or args.rounds < 1 or (args.backend and args.cpus != 2):
        parser.error("cpus must be 2..16; RTL backend is two ports; rounds positive")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps({"result": "RUNNING", "passed": False}) + "\n")
    dis = subprocess.check_output(["arm-none-eabi-objdump", "-d", str(args.elf)], text=True)
    assert not re.search(r"\b(?:ldrex|strex)(?:b|h|d)?\b", dis, re.I)
    bus = rtl.Bus(args.backend) if args.backend else None
    try:
        domain = ProgramDomain(args.elf, bus, args.cpus, args.seed)
        result = domain.run(args.rounds, reset_cpus=[1] if bus else None)
        assert all(n > 0 for n in domain.user_instructions[:2])
        assert all(n > 0 for n in domain.private_stack_checks[:2])
        assert {0x0002, 0x0207, 0x0208, 0x020e, 0x0b00, 0x0b01, 0x0b02, 0x0300, 0x0301, 0x7fff} <= domain.syscall_numbers
        assert domain.scalar("program_test_rejections") == 8
        assert any("[PROGRAM-THREAD] PASS final heap restored handles stacks image=freed" in log for log in domain.logs)
        assert {4, 5, 0x010d} <= domain.syscall_numbers
        assert any("[PROGRAM-RUNTIME] PASS immutable binding serial IDs survive join cross-cpu=1" in log for log in domain.logs)
        assert any("[PROGRAM-THREAD] PASS public affinity parent=1 child=0" in log for log in domain.logs)
        assert {3, 0x0100, 0x0202, 0x0205, 0x0209, 0x020a} <= domain.syscall_numbers
        assert any("[PROGRAM-THREAD] PASS nested-create join close shared-space stack-pin=1" in log for log in domain.logs)
        assert any("[PROGRAM-THREAD] PASS child survives leader exit image heap startup=live" in log for log in domain.logs)
        assert any("[PROGRAM-DOMAIN] PASS same kernel before/after CPU migration mask=3" in log for log in domain.logs)
        assert any("[PROGRAM-ALLOCATOR] PASS common SDK malloc/calloc/realloc/free" in log for log in domain.logs)
        assert any("[PROGRAM-HEAP] PASS checks=20 wait-pinned=1 cross-cpu=1" in log for log in domain.logs)
        result.update(user_instructions=domain.user_instructions, svc_entries=domain.svc_count,
                      exception_returns=domain.returns, psp_exception_private_stack_checks=domain.private_stack_checks, native_syscalls=sorted(domain.syscall_numbers),
                      seed=args.seed, quantum=1, elf_rejections=8,
                      tick_injection="at most one SysTick per 1000 interleaving rounds; not cycle timing",
                      elf_sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                      evidence="native ELF instructions, modeled Cortex-M exception hardware; not physical boards")
        if bus:
            assert int(bus.command("f")) == 5
            result.update(single_cpu_reset="CPU1 rejected; RTL fault=5",
                          rtl_bus_accesses=bus.accesses, rtl_instruction_checks=True)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(result, indent=2)+"\n")
        print(json.dumps(result, indent=2))
    finally:
        if bus: bus.close()


if __name__ == "__main__": main()
