#!/usr/bin/env python3
"""Run real ARMv7-M kernel ELFs on separate Cortex-M3 instruction engines.

Requires unicorn>=2.1 and pyelftools. Only bus/peripheral behavior is modeled;
boot, allocator, atomics, task lifecycle and scheduler execute firmware bytes.
This is functional emulation, never evidence of a physical board boot.
"""
import argparse
import ctypes
import hashlib
import json
import mmap
from pathlib import Path
import random
import re
import subprocess
import sys

try:
    from elftools.elf.elffile import ELFFile
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_PROT_ALL
    from unicorn.arm_const import (UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR,
        UC_ARM_REG_XPSR, UC_ARM_REG_PRIMASK, UC_CPU_ARM_CORTEX_M3)
except ImportError:
    sys.exit("Install test dependencies: python3 -m pip install unicorn pyelftools")

FLASH = 0x08000000
LOCAL = 0x20000000
SHARED = 0x60000000
FABRIC = 0x64000000


class Domain:
    def __init__(self, elf, cpus, seed, quantum):
        self.image = Path(elf).read_bytes()
        with open(elf, "rb") as f:
            obj = ELFFile(f)
            self.symbols = {s.name: int(s["st_value"])
                            for s in obj.get_section_by_name(".symtab").iter_symbols()}
            self.idle_size = next(int(s["st_size"]) for s in
                                  obj.get_section_by_name(".symtab").iter_symbols()
                                  if s.name == "idle_loop")
            self.loads = [(int(p["p_paddr"]), p.data()) for p in obj.iter_segments()
                          if p["p_type"] == "PT_LOAD" and int(p["p_filesz"])]
        self.n = cpus
        self.program_mode = "program_test_done" in self.symbols
        self.expected_exits = (14 if "program_test_compute_enabled" in self.symbols else (12 if "sys_a20_task_spawn" in self.symbols else 5)) if self.program_mode else 2
        self.quantum = quantum
        self.rng = random.Random(seed)
        self.shared_size = self.symbols["_heap_end"] - SHARED
        self.shared = mmap.mmap(-1, self.shared_size)
        self.shared[:] = b"\xcd" * self.shared_size
        self.backing = (ctypes.c_char * self.shared_size).from_buffer(self.shared)
        self.magic = 0x41323053
        self.magic_reads = [0] * cpus
        self.claimed = False
        self.attached = 0
        self.released = 0
        self.release_reads = [0] * cpus
        self.fault = 0
        self.ipi = 0
        self.ipi_sent = 0
        self.ipi_acked = 0
        self.gates = [None, None]
        self.contention = [0, 0]
        self.acquires = [0, 0]
        self.logs = ["" for _ in range(cpus)]
        self.engines = []
        self.peripherals = [{} for _ in range(cpus)]
        self.ticks = [0 for _ in range(cpus)]
        self.context = [None for _ in range(cpus)]
        self.sentinel = FLASH + 0x7ff00
        self.main_entries = [0] * cpus
        self.secondary_entries = [0] * cpus
        self.exits = [0] * cpus
        self.post_exit_idle = [0] * cpus
        self.instructions = [0] * cpus
        self.local_size = self.symbols["_ram_linked_end"] - LOCAL
        for cpu in range(cpus):
            engine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
            engine.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M3)
            engine.mem_map(FLASH, 512 * 1024)
            engine.mem_map(LOCAL, self.local_size)
            engine.mem_map_ptr(SHARED, self.shared_size, UC_PROT_ALL,
                              ctypes.addressof(self.backing))
            engine.mmio_map(FABRIC, 4096, self.fabric_read, cpu,
                            self.fabric_write, cpu)
            engine.mmio_map(0x40000000, 0x30000, self.io_read, cpu,
                            self.io_write, cpu)
            engine.mem_map(0xa0000000, 4096)  # FSMC configuration
            engine.mem_map(0xe000e000, 4096)  # NVIC / SysTick register state
            for address, data in self.loads:
                assert FLASH <= address < FLASH + 512 * 1024
                engine.mem_write(address, data)
            engine.reg_write(UC_ARM_REG_SP, self.symbols["_stack_top"])
            engine.reg_write(UC_ARM_REG_XPSR, 0x01000000)
            engine.reg_write(UC_ARM_REG_PC, self.symbols["Reset_Handler"])
            from unicorn import UC_HOOK_CODE
            engine.hook_add(UC_HOOK_CODE, self.entry_hook, cpu,
                            self.symbols["kernel_main"] & ~1,
                            self.symbols["kernel_main"] & ~1)
            engine.hook_add(UC_HOOK_CODE, self.secondary_hook, cpu,
                            self.symbols["smp_secondary_init"] & ~1,
                            self.symbols["smp_secondary_init"] & ~1)
            engine.hook_add(UC_HOOK_CODE, self.exit_hook, cpu,
                            self.symbols["proc_exit"] & ~1,
                            self.symbols["proc_exit"] & ~1)
            engine.hook_add(UC_HOOK_CODE, self.idle_hook, cpu,
                            self.symbols["idle_loop"] & ~1,
                            (self.symbols["idle_loop"] & ~1) + self.idle_size - 1)
            self.engines.append(engine)

    def entry_hook(self, uc, address, size, cpu):
        self.main_entries[cpu] += 1
        assert cpu == 0, "secondary entered kernel_main"

    def secondary_hook(self, uc, address, size, cpu):
        self.secondary_entries[cpu] += 1
        assert cpu > 0, "boot CPU entered secondary bootstrap"

    def exit_hook(self, uc, address, size, cpu):
        self.exits[cpu] += 1

    def idle_hook(self, uc, address, size, cpu):
        if sum(self.exits) == self.expected_exits:
            self.post_exit_idle[cpu] += 1

    def fabric_read(self, uc, offset, size, cpu):
        assert size == 2 or (offset == 0 and size == 4), (offset, size)
        if offset == 0:
            self.magic_reads[cpu] += 1
            return self.magic
        if offset == 4:
            return cpu
        if offset == 8:
            return self.n
        if offset == 12:
            if cpu != 0 or self.claimed:
                return 0
            self.claimed = True
            return 1
        if offset == 16:
            self.release_reads[cpu] += 1
            return self.released
        if offset == 20:
            return self.fault
        if offset == 24:
            if self.attached & (1 << cpu):
                return 0
            self.attached |= 1 << cpu
            return 1
        if offset == 36:
            bit = 1 << cpu
            old = self.ipi & bit
            self.ipi &= ~bit
            self.ipi_acked += bool(old)
            return bool(old)
        if 256 <= offset < 264 and offset % 4 == 0:
            lock = (offset - 256) // 4
            if self.gates[lock] is None:
                self.gates[lock] = cpu
                self.acquires[lock] += 1
                return 1
            self.contention[lock] += 1
            return 0
        raise AssertionError(("unknown fabric read", offset, size, cpu))

    def fabric_write(self, uc, offset, size, value, cpu):
        assert size == 2, (offset, size)
        if offset == 16:
            assert cpu == 0 and self.claimed
            self.released |= value
        elif offset == 20:
            self.fault = self.fault or value
        elif offset == 32:
            self.ipi |= value
            self.ipi_sent += 1
        elif 256 <= offset < 264 and offset % 4 == 0:
            lock = (offset - 256) // 4
            assert value == 1 and self.gates[lock] == cpu, "unlock without ownership"
            self.gates[lock] = None
        else:
            raise AssertionError(("unknown fabric write", offset, size, value, cpu))

    def io_read(self, uc, offset, size, cpu):
        if offset == 0x13800:  # USART1 SR, TX ready, no synthetic input
            return 0xc0
        return self.peripherals[cpu].get(offset, 0)

    def io_write(self, uc, offset, size, value, cpu):
        if offset == 0x13804:
            char = chr(value & 255)
            self.logs[cpu] += char
            if char == "\n":
                print(f"cpu{cpu}: {self.logs[cpu].splitlines()[-1]}", flush=True)
        self.peripherals[cpu][offset] = value

    def scalar(self, name, size=4):
        address = self.symbols[name]
        return int.from_bytes(self.shared[address-SHARED:address-SHARED+size], "little")

    def step(self, cpu, inject_tick):
        engine = self.engines[cpu]
        csr = int.from_bytes(engine.mem_read(0xe000e010, 4), "little")
        if (inject_tick and self.context[cpu] is None and csr & 3 == 3
                and not engine.reg_read(UC_ARM_REG_PRIMASK)):
            # Model exception entry/return around the real vector's handler.
            # Preserve interrupted registers and reserve the hardware frame;
            # IRQ execution may span many interleaved CPU quanta while waiting
            # on a hardware semaphore. No scheduler function is host-injected.
            self.context[cpu] = engine.context_save()
            sp = engine.reg_read(UC_ARM_REG_SP)
            engine.reg_write(UC_ARM_REG_SP, (sp - 32) & ~7)
            engine.reg_write(UC_ARM_REG_LR, self.sentinel | 1)
            engine.reg_write(UC_ARM_REG_PC, self.symbols["SysTick_Handler"])
            engine.reg_write(UC_ARM_REG_PRIMASK, 1)
            # Hardware exception entry clears interrupted Thumb IT state.
            # Keep it only in the saved context for exception return.
            psr = engine.reg_read(UC_ARM_REG_XPSR)
            psr &= ~((0x3f << 10) | (3 << 25) | 0x1ff)
            engine.reg_write(UC_ARM_REG_XPSR, psr | 0x0100000f)
            self.ticks[cpu] += 1
        pc = engine.reg_read(UC_ARM_REG_PC)
        self.instructions[cpu] += self.quantum  # upper bound, not retired count
        try:
            engine.emu_start(pc | 1, self.sentinel, count=self.quantum)
        except Exception as exc:
            current = engine.reg_read(UC_ARM_REG_PC)
            # Unicorn 2.1.4 reports Thumb YIELD as unsupported after advancing
            # PC. YIELD is a scheduling hint: the engine already yielded its
            # instruction quantum, so resume unchanged on the next turn.
            from unicorn import UcError, UC_ERR_INSN_INVALID
            if (isinstance(exc, UcError) and exc.errno == UC_ERR_INSN_INVALID
                    and bytes(engine.mem_read(current - 2, 2)) == b"\x10\xbf"):
                return
            raise RuntimeError(f"cpu={cpu} pc={current:#x} sp={engine.reg_read(UC_ARM_REG_SP):#x} bytes={engine.mem_read(current, 8).hex()}") from exc
        if engine.reg_read(UC_ARM_REG_PC) == self.sentinel:
            assert self.context[cpu] is not None
            engine.context_restore(self.context[cpu])
            self.context[cpu] = None
        for text in self.logs:
            assert "[PANIC] arch=" not in text and "fault exception=" not in text, text

    def run(self, rounds, reset_cpus=None):
        # Start CPU1 first: it must wait without touching poisoned shared BSS.
        before = bytes(self.shared)
        for cpu in range(1, self.n):
            for _ in range(100000):
                self.step(cpu, False)
                if self.release_reads[cpu]:
                    break
            else:
                raise AssertionError("secondary did not reach release wait")
        assert bytes(self.shared) == before, "secondary initialized shared RAM"
        order = list(range(self.n))
        for cycle in range(rounds):
            self.rng.shuffle(order)
            for cpu in order:
                self.step(cpu, cycle % (1000 if self.program_mode else 4) == 0)
            assert not self.fault, f"fabric fault={self.fault}"
            marker = "SVC=native released=all\r\n" if self.program_mode else "heap=cross-cpu\r\n"
            done = "program_test_done" if self.program_mode else "done"
            if (marker in "".join(self.logs)
                    and self.scalar(done) == 1 and sum(self.exits) == self.expected_exits
                    and self.post_exit_idle[0] > 0):
                break
        else:
            details = [(i, hex(e.reg_read(UC_ARM_REG_PC)),
                        hex(e.reg_read(UC_ARM_REG_SP))) for i,e in enumerate(self.engines)]
            phase = self.scalar("program_test_done" if self.program_mode else "phase")
            raise AssertionError(f"timeout: phase={phase} {details} ticks={self.ticks} global_ticks={self.scalar('stm32_ticks', 8)} gates={self.gates} contexts={[c is not None for c in self.context]} primask={[e.reg_read(UC_ARM_REG_PRIMASK) for e in self.engines]}")
        assert self.main_entries == [1] + [0] * (self.n - 1), self.main_entries
        assert self.secondary_entries == [0] + [1] * (self.n - 1)
        if self.program_mode:
            assert self.scalar("mcu_program_live") == 0
        else:
            assert self.scalar("count32") == 256
            assert self.scalar("count64", 8) == 0x100000080
        assert self.contention[0] > 0, "no interleaved atomic contention observed"
        assert self.ipi_sent > 0 and self.ipi_acked > 0
        # Reset each MCU separately. Sticky attachment must reject re-entry
        # without clearing shared task/heap bytes. Faulting port never writes
        # through the shared-data initialization path again.
        for cpu in (reversed(range(self.n)) if reset_cpus is None else reset_cpus):
            snapshot = bytes(self.shared)
            engine = self.engines[cpu]
            engine.reg_write(UC_ARM_REG_SP, self.symbols["_stack_top"])
            engine.reg_write(UC_ARM_REG_PC, self.symbols["Reset_Handler"])
            engine.reg_write(UC_ARM_REG_PRIMASK, 1)
            self.context[cpu] = None
            for _ in range(100000):
                self.step(cpu, False)
                if self.fault:
                    break
            assert self.fault == 5, "warm reset was not rejected"
            assert bytes(self.shared) == snapshot, "warm reset destroyed shared state"
            self.fault = 0  # test fixture only; real domain requires cold reset
        return {"elf_sha256": hashlib.sha256(self.image).hexdigest(),
                "cpu_count": self.n, "main_entries": self.main_entries,
                "secondary_entries": self.secondary_entries, "task_exits": self.exits,
                "post_exit_idle_instructions": self.post_exit_idle,
                "rounds": cycle + 1, "instruction_budget": self.instructions,
                "ticks": self.ticks, "hardware_lock_acquires": self.acquires,
                "hardware_lock_contention": self.contention,
                "ipi_sent": self.ipi_sent, "ipi_acked": self.ipi_acked,
                "single_cpu_reset": "rejected, shared memory unchanged",
                "secondary_wait_verified": self.release_reads,
                "result": "PASS", "evidence": "instruction emulation, not physical hardware"}


def boot_rejection_tests(elf, cpus, seed, quantum):
    results = {}
    missing = Domain(elf, cpus, seed, quantum)
    missing.magic = 0
    before = bytes(missing.shared)
    for _ in range(100000):
        missing.step(0, False)
        if missing.magic_reads[0]:
            break
    assert missing.magic_reads[0], "missing-fabric test never reached probe"
    for _ in range(100):
        missing.step(0, False)
    assert not missing.claimed and not missing.attached and not missing.fault
    assert bytes(missing.shared) == before and not any(missing.logs)
    results["invalid_fabric_magic"] = "halted without shared access"

    mismatch = Domain(elf, cpus + 1, seed, quantum)
    before = bytes(mismatch.shared)
    for _ in range(100000):
        mismatch.step(0, False)
        if mismatch.fault:
            break
    assert mismatch.fault == 4 and not mismatch.claimed
    assert bytes(mismatch.shared) == before
    results["topology_mismatch"] = "rejected before shared initialization"

    early = Domain(elf, cpus, seed, quantum)
    before = bytes(early.shared)
    engine = early.engines[0]
    engine.reg_write(UC_ARM_REG_PC, early.symbols["HardFault_Handler"])
    engine.reg_write(UC_ARM_REG_XPSR, 0x01000003)
    for _ in range(100):
        early.step(0, False)
    assert bytes(early.shared) == before and not any(early.logs)
    assert early.acquires == [0, 0] and not early.claimed
    results["early_bus_fault"] = "halted without console/atomic gates"
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--seed", type=int, default=20)
    parser.add_argument("--quantum", type=int, default=97)
    parser.add_argument("--rounds", type=int, default=200000)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--objdump", default="arm-none-eabi-objdump")
    args = parser.parse_args()
    if not 2 <= args.cpus <= 16 or args.quantum < 1 or args.rounds < 1:
        parser.error("cpus must be 2..16; quantum and rounds must be positive")
    disassembly = subprocess.check_output([args.objdump, "-d", str(args.elf)], text=True)
    if re.search(r"\b(?:ldrex|strex)(?:b|h|d)?\b", disassembly, re.I):
        raise AssertionError("chip-local exclusive instructions in shared firmware")
    result = Domain(args.elf, args.cpus, args.seed, args.quantum).run(args.rounds)
    result["exclusive_instruction_audit"] = "PASS, no LDREX/STREX"
    result["boot_rejection_tests"] = boot_rejection_tests(
        args.elf, args.cpus, args.seed, args.quantum)
    result.update(seed=args.seed, quantum=args.quantum)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
