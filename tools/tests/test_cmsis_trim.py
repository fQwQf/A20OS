"""CMSIS-RTOS2 personality trim: manifest, generator and ELF verifier."""
from __future__ import annotations

import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cmsis_trim

ROOT = Path(__file__).resolve().parents[2]


class CmsisTrim(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest = cmsis_trim.load_manifest()
        self.master = cmsis_trim.parse_master()

    def test_every_profile_syscall_has_a_reason(self) -> None:
        reasons = self.manifest["syscall_reason"]
        for name, profile in self.manifest["profile"].items():
            if profile.get("syscalls") == "all":
                continue  # the full profile deliberately mirrors the master
            with self.subTest(profile=name):
                for call in cmsis_trim.resolve_syscalls(
                        self.manifest, profile, self.master):
                    self.assertIn(call, reasons)

    def test_mcu_profiles_are_subsets_of_full(self) -> None:
        full = set(cmsis_trim.resolve_syscalls(
            self.manifest, self.manifest["profile"]["full"], self.master))
        for name in ("mcu", "mcu-min"):
            calls = set(cmsis_trim.resolve_syscalls(
                self.manifest, self.manifest["profile"][name], self.master))
            with self.subTest(profile=name):
                self.assertTrue(calls <= full)

    def test_mcu_min_keeps_only_the_demo_object_set(self) -> None:
        config = cmsis_trim.render_config(
            self.manifest, "mcu-min", self.manifest["profile"]["mcu-min"])
        self.assertIn("#define OS_CFG_THREAD 1", config)
        self.assertIn("#define OS_CFG_SEMAPHORE 1", config)
        self.assertIn("#define OS_CFG_TIMER 1", config)
        self.assertIn("#define OS_CFG_MUTEX 0", config)
        self.assertIn("#define OS_CFG_MEMORY_POOL 0", config)

    def test_emit_writes_a_subset_table_in_master_order(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = cmsis_trim.emit("mcu-min", Path(tmp))
            table = out["table"].read_text(encoding="utf-8")
            self.assertIn(
                "A20_NATIVE_SYSCALL(clock_get, sys_a20_clock_get(args))",
                table)
            self.assertNotIn("task_wait", table)
            order = [line for line in table.splitlines()
                     if line.startswith("A20_NATIVE_SYSCALL(")]
            master_order = list(self.master)
            positions = [master_order.index(
                line.split("(")[1].split(",")[0].strip()) for line in order]
            self.assertEqual(positions, sorted(positions))

    def _fake_nm(self, tmp: str, symbols: list[str]) -> str:
        path = Path(tmp) / "fake-nm"
        body = "\n".join(f"00000000 T sys_a20_{s}" for s in symbols)
        path.write_text(f"#!/bin/sh\ncat <<'EOF'\n{body}\nEOF\n")
        path.chmod(0o755)
        return str(path)

    def test_check_accepts_exactly_the_profile(self) -> None:
        calls = cmsis_trim.resolve_syscalls(
            self.manifest, self.manifest["profile"]["mcu-min"], self.master)
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / "kernel.elf"
            elf.write_bytes(b"\x7fELF")
            nm = self._fake_nm(tmp, calls)
            result = cmsis_trim.check("mcu-min", elf, nm)
            self.assertEqual(result["count"], len(calls))

    def test_check_rejects_an_unlisted_syscall(self) -> None:
        calls = cmsis_trim.resolve_syscalls(
            self.manifest, self.manifest["profile"]["mcu-min"], self.master)
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / "kernel.elf"
            elf.write_bytes(b"\x7fELF")
            nm = self._fake_nm(tmp, calls + ["task_wait"])
            with self.assertRaises(cmsis_trim.TrimError):
                cmsis_trim.check("mcu-min", elf, nm)

    def test_check_rejects_a_missing_syscall(self) -> None:
        calls = cmsis_trim.resolve_syscalls(
            self.manifest, self.manifest["profile"]["mcu-min"], self.master)
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / "kernel.elf"
            elf.write_bytes(b"\x7fELF")
            nm = self._fake_nm(tmp, calls[:-1])
            with self.assertRaises(cmsis_trim.TrimError):
                cmsis_trim.check("mcu-min", elf, nm)

    def test_native_platform_calls_match_the_mcu_manifest(self) -> None:
        # native_platform_a20.c is the only file that names A20 syscalls; its
        # call set therefore is the backend's real syscall surface.
        src = (ROOT / "user/cmsis_rtos2/src/native_platform_a20.c").read_text(
            encoding="utf-8")
        called = set(re.findall(r"\ba20_([a-z0-9_]+)\s*\(", src))
        called -= {"malloc", "free"}          # allocator wrappers -> vm_alloc
        called.discard("time_ns_t")           # type name
        mapping = {
            "sched_set": "task_set_sched",
            "sched_get": "task_get_sched",
            "hdl_close": "handle_close",
            "vm_alloc_pages": "vm_alloc",   # wrapper over the same syscall
        }
        mapped = {mapping.get(name, name) for name in called}
        mapped.add("vm_alloc")                # a20_malloc grows the heap
        profile = set(cmsis_trim.resolve_syscalls(
            self.manifest, self.manifest["profile"]["mcu"], self.master))
        self.assertEqual(mapped, profile)

    def test_unknown_profile_is_rejected(self) -> None:
        with self.assertRaises(cmsis_trim.TrimError):
            cmsis_trim.get_profile(self.manifest, "does-not-exist")

    def test_generated_table_includes_the_nr_header_once(self) -> None:
        table = cmsis_trim.render_def(
            "mcu", cmsis_trim.resolve_syscalls(
                self.manifest, self.manifest["profile"]["mcu"], self.master),
            self.master)
        self.assertIn('#include "abi/native/syscall_nr.h"', table)
        self.assertNotIn("#ifndef SYSCALL_TABLE", table)


if __name__ == "__main__":
    unittest.main()
