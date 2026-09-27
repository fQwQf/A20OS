"""Regression tests for the a20 instance toolchain.

Stdlib unittest on purpose: `tools/a20` declares zero third-party dependencies
(PEP 723 `dependencies = []`) and docs/instances.md promises stdlib only, so a
pytest requirement would quietly break that promise for every contributor.

Most tests are hermetic -- they write a manifest into a temp dir and assert the
parse/validate/derive pipeline.  They deliberately do not read instances/*.toml,
so editing an instance cannot break the tool's own tests; TestRepositoryInstances
is the one place that intentionally reads the real tree.
"""

from __future__ import annotations

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from a20_board import FLASH_TARGET_REACHABLE, run_flash  # noqa: E402
from a20_derive import derive_make_vars  # noqa: E402
from a20_instance import InstanceError, parse_instance  # noqa: E402
from a20_make import REPO_ROOT  # noqa: E402
from a20_resource import (  # noqa: E402
    DEFAULT_RESERVE_MEM_MB,
    HostResources,
    MemorySpecError,
    Policy,
    Requirement,
    evaluate,
    parse_memory_mb,
    requirement_for,
)
from a20_registry import (  # noqa: E402
    RegistryError,
    load_flash_backends,
    load_registry,
    validate_flash_backends,
    validate_registry,
)
from a20_validate import validate_instance  # noqa: E402


def load_instance(name: str):
    return parse_instance(REPO_ROOT / "instances" / f"{name}.toml")


def load(tmp: Path, text: str):
    path = tmp / "case.toml"
    path.write_text(textwrap.dedent(text).lstrip(), encoding="utf-8")
    return parse_instance(path)


def derived(tmp: Path, text: str) -> dict[str, str]:
    return dict(v.split("=", 1) for v in derive_make_vars(load(tmp, text)))


class TestDeriveMakeVars(unittest.TestCase):
    """The field -> make-variable contract documented in docs/instances.md.

    Each case pins one documented mapping.  A silent rename here is invisible
    otherwise: the instance still validates, the variable just stops being set,
    and the Makefile quietly falls back to its own default.
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_minimal_instance_emits_only_arch_and_board(self) -> None:
        self.assertEqual(derived(self.tmp, 'arch = "riscv64"\n'),
                         {"ARCH": "riscv64", "BOARD": "qemu-virt-riscv64"})

    def test_unset_fields_emit_nothing(self) -> None:
        """Absent fields must fall through to the Makefile default, not emit a value."""
        got = derived(self.tmp, 'arch = "riscv64"\n')
        for absent in ("ABI", "NR_CPUS", "PROFILE", "QEMU_MEMORY", "SMOKE_TIMEOUT"):
            self.assertNotIn(absent, got)

    def test_armv7m_defaults_to_stm32f103_board(self) -> None:
        self.assertEqual(derived(self.tmp, 'arch = "armv7m"\n')["BOARD"], "stm32f103")

    def test_abi_and_machine(self) -> None:
        got = derived(self.tmp, """
            arch = "riscv64"
            abi = "linux"
            [machine]
            smp = 4
            memory = "2G"
        """)
        self.assertEqual(got["ABI"], "linux")
        self.assertEqual(got["NR_CPUS"], "4")
        self.assertEqual(got["QEMU_MEMORY"], "2G")

    def test_bool_kernel_flags(self) -> None:
        """CONFIG_SWAP is Kconfig-style y/n; the make-style flags are 1/0.

        The split is deliberate and documented (docs/instances.md notes swap as
        y/n), so it is pinned here rather than normalised -- changing one
        spelling silently disables the feature.
        """
        got = derived(self.tmp, """
            arch = "riscv64"
            [kernel]
            nommu = true
            ubsan = false
            swap = true
        """)
        self.assertEqual(got["NOMMU"], "1")
        self.assertEqual(got["CONFIG_UBSAN"], "0")
        self.assertEqual(got["CONFIG_SWAP"], "y")

        off = derived(self.tmp, 'arch = "riscv64"\n[kernel]\nubsan = true\nswap = false\n')
        self.assertEqual(off["CONFIG_UBSAN"], "1")
        self.assertEqual(off["CONFIG_SWAP"], "n")

    def test_nommu_maps_to_NOMMU_not_CONFIG(self) -> None:
        got = derived(self.tmp, 'arch = "riscv64"\n[kernel]\nnommu = true\n')
        self.assertEqual(got.get("NOMMU"), "1")
        self.assertNotIn("CONFIG_NOMMU", got)

    def test_stm32_fields(self) -> None:
        got = derived(self.tmp, """
            arch = "armv7m"
            [stm32]
            flash_kb = 512
            ram_kb = 64
            xuanwu = true
        """)
        self.assertEqual(got["STM32_FLASH_KB"], "512")
        self.assertEqual(got["STM32_RAM_KB"], "64")
        self.assertEqual(got["STM32_XUANWU"], "1")

    def test_flash_fields(self) -> None:
        got = derived(self.tmp, """
            arch = "armv7m"
            [flash]
            tool = "openocd"
            interface = "interface/cmsis-dap.cfg"
            transport = "swd"
            adapter_khz = 1000
            serial = "PROBE1"
        """)
        self.assertEqual(got["STM32_OPENOCD_INTERFACE"], "interface/cmsis-dap.cfg")
        self.assertEqual(got["STM32_OPENOCD_TRANSPORT"], "swd")
        self.assertEqual(got["STM32_OPENOCD_ADAPTER_KHZ"], "1000")
        self.assertEqual(got["STM32_CMSIS_DAP_SERIAL"], "PROBE1")

    def test_hostfwd_list_becomes_comma_joined(self) -> None:
        got = derived(self.tmp, """
            arch = "riscv64"
            [net]
            hostfwd = ["tcp::5555-:5555", "udp::5555-:5555"]
        """)
        self.assertEqual(got["NET_HOSTFWD"], "tcp::5555-:5555,udp::5555-:5555")

    def test_test_section(self) -> None:
        got = derived(self.tmp, """
            arch = "riscv64"
            [test]
            timeout = "45s"
            input_delay = 3
        """)
        self.assertEqual(got["SMOKE_TIMEOUT"], "45s")
        self.assertEqual(got["SMOKE_INPUT_DELAY"], "3")

    def test_rootfs_sizes(self) -> None:
        got = derived(self.tmp, """
            arch = "riscv64"
            [rootfs]
            size_mb = 256
            ext4_size_mb = 64
            extra_size_mb = 512
        """)
        self.assertEqual(got["FAT32_IMAGE_MB"], "256")
        self.assertEqual(got["EXT4_IMAGE_MB"], "64")
        self.assertEqual(got["EXTRA_IMAGE_MB"], "512")

    def test_a20_consumed_fields_emit_no_variables(self) -> None:
        """gui.enabled, extra_qemu, test.commands/expect are a20's own, not make's."""
        got = derived(self.tmp, """
            arch = "riscv64"
            [gui]
            enabled = true
            [machine]
            extra_qemu = ["-device", "riscv-iommu-pci,bus=pcie.0"]
            [test]
            commands = ["syscall_smoke"]
            expect = ["PASS"]
        """)
        for absent in ("QEMU_GUI", "EXTRA_QEMU", "SMOKE_COMMANDS", "SMOKE_EXPECT"):
            self.assertNotIn(absent, got)


class TestParseInstance(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_missing_arch_is_rejected(self) -> None:
        with self.assertRaises(InstanceError) as cm:
            load(self.tmp, 'name = "x"\n')
        self.assertIn("arch", str(cm.exception))

    def test_unknown_top_level_key_is_rejected(self) -> None:
        with self.assertRaises(InstanceError) as cm:
            load(self.tmp, 'arch = "riscv64"\nbogus = 1\n')
        self.assertIn("bogus", str(cm.exception))

    def test_unknown_section_key_is_rejected(self) -> None:
        """docs/instances.md documented gui.frame_window and rootfs.gui_size_mb,
        which the parser rejected as unknown keys.  Rejecting unknown keys is
        the behaviour we want; this test pins that a *new* key is caught too."""
        with self.assertRaises(InstanceError) as cm:
            load(self.tmp, 'arch = "riscv64"\n[gui]\nframe_window = 15\n')
        self.assertIn("frame_window", str(cm.exception))

    def test_wrong_scalar_type_is_rejected(self) -> None:
        with self.assertRaises(InstanceError) as cm:
            load(self.tmp, 'arch = "riscv64"\n[machine]\nsmp = "four"\n')
        self.assertIn("smp", str(cm.exception))

    def test_bool_is_not_accepted_where_int_expected(self) -> None:
        with self.assertRaises(InstanceError):
            load(self.tmp, 'arch = "riscv64"\n[stm32]\nflash_kb = true\n')

    def test_name_defaults_to_file_stem(self) -> None:
        inst = load(self.tmp, 'arch = "riscv64"\n')
        self.assertEqual(inst.name, "case")

    def test_every_error_is_reported_not_just_the_first(self) -> None:
        with self.assertRaises(InstanceError) as cm:
            load(self.tmp, 'arch = 1\nbogus = 2\n')
        self.assertGreaterEqual(len(cm.exception.errors), 2)


class TestValidateInstance(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def check(self, text: str) -> list[str]:
        return validate_instance(load(self.tmp, text), REPO_ROOT)

    def test_sane_instance_has_no_errors(self) -> None:
        self.assertEqual(self.check('arch = "riscv64"\n'), [])

    def test_unknown_arch(self) -> None:
        self.assertTrue(any("arch" in e for e in self.check('arch = "z80"\n')))

    def test_smp_needs_opt_in_off_qemu(self) -> None:
        self.assertTrue(self.check('arch = "riscv64"\nboard = "visionfive2"\n'
                                   '[machine]\nsmp = 2\n'))
        self.assertEqual(self.check('arch = "riscv64"\nboard = "visionfive2"\n'
                                    '[machine]\nsmp = 2\nallow_unverified_smp = true\n'), [])

    def test_nommu_is_arch_gated(self) -> None:
        self.assertTrue(self.check('arch = "x86_64"\n[kernel]\nnommu = true\n'))
        self.assertEqual(self.check('arch = "riscv64"\n[kernel]\nnommu = true\n'), [])

    def test_gui_conflicts_with_bringup_and_test(self) -> None:
        self.assertTrue(self.check('arch = "riscv64"\n[kernel]\nbringup = true\n'
                                   '[gui]\nenabled = true\n'))

    def test_test_requires_expect(self) -> None:
        self.assertTrue(self.check('arch = "riscv64"\n[test]\ntimeout = "20s"\n'))

    def test_stm32_section_is_armv7m_only(self) -> None:
        self.assertTrue(self.check('arch = "riscv64"\n[stm32]\nflash_kb = 64\n'))


class TestFlashBackendSafety(unittest.TestCase):
    """The board/geometry allowlist guards a destructive erase, so it gets the
    most adversarial coverage in this file: each case is a manifest that, before
    the registry existed, would have reached the 512 KiB xuanwu recipe."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.backends = load_flash_backends(REPO_ROOT)

    def check(self, text: str) -> list[str]:
        return validate_instance(load(self.tmp, text), REPO_ROOT)

    def test_registry_is_self_consistent(self) -> None:
        self.assertEqual(validate_flash_backends(self.backends, REPO_ROOT), [])

    def test_every_backend_target_is_dispatchable(self) -> None:
        for b in self.backends:
            self.assertIn(b.make_target, FLASH_TARGET_REACHABLE)

    def test_xuanwu_manifest_is_accepted(self) -> None:
        self.assertEqual(self.check("""
            arch = "armv7m"
            [stm32]
            flash_kb = 512
            ram_kb = 64
            xuanwu = true
            [flash]
            tool = "openocd"
        """), [])

    def test_64k_part_is_refused_the_512k_recipe(self) -> None:
        errs = self.check("""
            arch = "armv7m"
            [stm32]
            flash_kb = 64
            ram_kb = 20
            [flash]
            tool = "openocd"
        """)
        self.assertTrue(any("flash geometry" in e for e in errs), errs)

    def test_unregistered_backend_is_refused(self) -> None:
        errs = self.check('arch = "armv7m"\n[flash]\ntool = "totally-not-a-backend"\n')
        self.assertTrue(any("not a registered backend" in e for e in errs), errs)

    def test_backend_matched_to_the_wrong_board_is_refused(self) -> None:
        errs = self.check('arch = "riscv64"\nboard = "visionfive2"\n'
                           '[flash]\ntool = "openocd"\n')
        self.assertTrue(any("not validated for board" in e for e in errs), errs)

    def test_flash_section_without_tool_is_refused(self) -> None:
        self.assertTrue(self.check('arch = "armv7m"\n[flash]\nadapter_khz = 1000\n'))

    def test_geometry_mismatch_reports_both_fields(self) -> None:
        backend = self.backends[0]
        self.assertIn("flash_kb", backend.geometry_mismatch(64, 20) or "")
        self.assertIn("ram_kb", backend.geometry_mismatch(64, 20) or "")

    def test_geometry_check_ignores_unset_manifest_fields(self) -> None:
        """A manifest that does not state its geometry is not a mismatch; the
        recipe's own defaults still apply.  Only a stated contradiction fails."""
        backend = self.backends[0]
        self.assertIsNone(backend.geometry_mismatch(None, None))


class TestRegistryParsing(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def write(self, text: str) -> Path:
        path = self.tmp / "r.toml"
        path.write_text(textwrap.dedent(text).lstrip(), encoding="utf-8")
        return path

    def test_driver_registry_is_self_consistent(self) -> None:
        self.assertEqual(validate_registry(load_registry(REPO_ROOT), REPO_ROOT), [])

    def test_unknown_key_is_reported(self) -> None:
        with self.assertRaises(RegistryError) as cm:
            load_flash_backends_in(self.write('[[backend]]\nname = "x"\n'
                                              'boards = ["b"]\nmake_target = "t"\nbogus = 1\n'))
        self.assertIn("bogus", str(cm.exception))

    def test_missing_required_field_is_reported(self) -> None:
        with self.assertRaises(RegistryError) as cm:
            load_flash_backends_in(self.write('[[backend]]\nname = "x"\nboards = ["b"]\n'))
        self.assertIn("make_target", str(cm.exception))

    def test_empty_boards_list_is_reported(self) -> None:
        with self.assertRaises(RegistryError) as cm:
            load_flash_backends_in(self.write('[[backend]]\nname = "x"\n'
                                              'boards = []\nmake_target = "t"\n'))
        self.assertIn("boards", str(cm.exception))

    def test_wrong_type_is_reported(self) -> None:
        with self.assertRaises(RegistryError) as cm:
            load_flash_backends_in(self.write('[[backend]]\nname = "x"\n'
                                              'boards = "b"\nmake_target = "t"\n'))
        self.assertIn("boards", str(cm.exception))

    def test_nonexistent_make_target_is_caught(self) -> None:
        path = self.write('[[backend]]\nname = "x"\nboards = ["stm32f103"]\n'
                          'make_target = "definitely-not-a-target"\n')
        errors = validate_flash_backends(load_flash_backends_in(path), REPO_ROOT)
        self.assertTrue(any("not defined in any makefile" in e for e in errors), errors)

    def test_nonexistent_board_is_caught(self) -> None:
        path = self.write('[[backend]]\nname = "x"\nboards = ["no-such-board"]\n'
                          'make_target = "flash-xuanwu-openocd"\n')
        errors = validate_flash_backends(load_flash_backends_in(path), REPO_ROOT)
        self.assertTrue(any("has no kernel/platform" in e for e in errors), errors)


def load_flash_backends_in(path: Path):
    """load_flash_backends against an arbitrary path, for malformed-input tests."""
    import a20_registry
    original = a20_registry.flash_backends_path
    a20_registry.flash_backends_path = lambda _root: path
    try:
        return a20_registry.load_flash_backends(REPO_ROOT)
    finally:
        a20_registry.flash_backends_path = original


class TestRunFlashDispatch(unittest.TestCase):
    """run_flash carries its own copy of the board/geometry guard as defence in
    depth, so it needs its own coverage: the validate_instance tests above only
    prove the check-time path, and dropping the action-time guard must fail
    something."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.backends = load_flash_backends(REPO_ROOT)
        self.build = patch("a20_board.build_instance", return_value=0)
        self.exec_ = patch("a20_board.exec_make", return_value=0)
        self.build_m = self.build.start()
        self.exec_m = self.exec_.start()
        self.addCleanup(self.build.stop)
        self.addCleanup(self.exec_.stop)

    def flash(self, text: str) -> int:
        return run_flash(load(self.tmp, text), self.backends, [], dry_run=True)

    def test_mismatched_geometry_is_refused_before_any_build(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            self.flash("""
                arch = "armv7m"
                [stm32]
                flash_kb = 64
                ram_kb = 20
                [flash]
                tool = "openocd"
            """)
        self.assertIn("geometry", str(cm.exception))
        self.build_m.assert_not_called()
        self.exec_m.assert_not_called()

    def test_wrong_board_is_refused_before_any_build(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            self.flash('arch = "riscv64"\nboard = "visionfive2"\n[flash]\ntool = "openocd"\n')
        self.assertIn("not validated for board", str(cm.exception))
        self.build_m.assert_not_called()

    def test_unregistered_backend_is_refused(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            self.flash('arch = "armv7m"\n[flash]\ntool = "nope"\n')
        self.assertIn("not a registered backend", str(cm.exception))

    def test_matching_manifest_dispatches_to_the_registered_target(self) -> None:
        self.flash("""
            arch = "armv7m"
            [stm32]
            flash_kb = 512
            ram_kb = 64
            xuanwu = true
            [flash]
            tool = "openocd"
        """)
        self.build_m.assert_called_once()
        self.assertEqual(self.exec_m.call_args.args[1], "flash-xuanwu-openocd")

    def test_build_failure_short_circuits_before_programming(self) -> None:
        self.build_m.return_value = 2
        rc = self.flash("""
            arch = "armv7m"
            [stm32]
            flash_kb = 512
            ram_kb = 64
            [flash]
            tool = "openocd"
        """)
        self.assertEqual(rc, 2)
        self.exec_m.assert_not_called()


class TestRepositoryInstances(unittest.TestCase):
    """Integration sweep over the real tree.  This is the one place that reads
    instances/*.toml on purpose: it catches a manifest that stops parsing even
    when nobody runs `make check-instances`."""

    def test_every_instance_parses_and_validates(self) -> None:
        entries = load_registry(REPO_ROOT)
        failures: list[str] = []
        for path in sorted((REPO_ROOT / "instances").glob("*.toml")):
            try:
                inst = parse_instance(path)
            except InstanceError as e:
                failures.append(f"{path.name}: parse: {e}")
                continue
            for err in validate_instance(inst, REPO_ROOT):
                failures.append(f"{path.name}: {err}")
        self.assertEqual(failures, [], "\n".join(failures))

    def test_instance_names_are_unique(self) -> None:
        seen: dict[str, str] = {}
        for path in sorted((REPO_ROOT / "instances").glob("*.toml")):
            name = parse_instance(path).name
            self.assertNotIn(name, seen, f"{name} declared by both {seen.get(name)} and {path.name}")
            seen[name] = path.name


if __name__ == "__main__":
    unittest.main()


class TestParseMemory(unittest.TestCase):
    def test_qemu_spellings(self) -> None:
        for text, want in (("1G", 1024), ("512M", 512), ("2GiB", 2048), ("1024", 1024),
                           ("1.5G", 1536), ("2T", 2097152), ("64K", 1), (" 1g ", 1024)):
            self.assertEqual(parse_memory_mb(text), want, text)

    def test_fractional_mib_rounds_up(self) -> None:
        self.assertEqual(parse_memory_mb("0.5M"), 1)

    def test_garbage_is_rejected(self) -> None:
        for text in ("", "abc", "1 GB x", "1GiBx", "-1G", "G"):
            with self.assertRaises(MemorySpecError, msg=text):
                parse_memory_mb(text)

    def test_zero_does_not_produce_a_zero_budget(self) -> None:
        self.assertGreaterEqual(parse_memory_mb("0"), 1)


class TestResourceGate(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.policy = Policy(reserve_mem_mb=1024, min_disk_mb=2048, max_concurrent=4,
                             wait_timeout_s=0.0)

    def host(self, **kw) -> HostResources:
        base = dict(mem_available_mb=16384, cpu_count=16, load1=1.0,
                    disk_free_mb=40000, running_guests=0)
        base.update(kw)
        return HostResources(**base)

    def need(self, **kw) -> Requirement:
        base = dict(mem_mb=2048, cpus=2, disk_mb=4096)
        base.update(kw)
        return Requirement(**base)

    def test_ample_host_passes(self) -> None:
        self.assertTrue(evaluate(self.need(), self.host(), self.policy).ok)

    def test_memory_shortfall_is_reported(self) -> None:
        v = evaluate(self.need(), self.host(mem_available_mb=1500), self.policy)
        self.assertFalse(v.ok)
        self.assertIn("memory", v.reason())

    def test_memory_uses_available_not_free(self) -> None:
        """A host with almost all memory in page cache is available, not full.

        Reading `free` here would report a few hundred MiB and block forever on
        an idle machine.
        """
        self.assertTrue(evaluate(self.need(), self.host(mem_available_mb=25600),
                                 self.policy).ok)

    def test_cpu_pressure_is_reported(self) -> None:
        v = evaluate(self.need(cpus=16), self.host(load1=4.0), self.policy)
        self.assertFalse(v.ok)
        self.assertIn("cpu", v.reason())

    def test_disk_shortfall_is_reported(self) -> None:
        v = evaluate(self.need(), self.host(disk_free_mb=500), self.policy)
        self.assertFalse(v.ok)
        self.assertIn("disk", v.reason())

    def test_guest_slot_cap_is_enforced(self) -> None:
        v = evaluate(self.need(), self.host(running_guests=4), self.policy)
        self.assertFalse(v.ok)
        self.assertIn("guest slots", v.reason())

    def test_slot_cap_defaults_to_a_quarter_of_cpus(self) -> None:
        loose = Policy(max_concurrent=0)
        self.assertTrue(evaluate(self.need(), self.host(running_guests=3), loose).ok)
        self.assertFalse(evaluate(self.need(), self.host(running_guests=4), loose).ok)

    def test_all_shortfalls_are_listed_together(self) -> None:
        v = evaluate(self.need(), self.host(mem_available_mb=10, disk_free_mb=10,
                                            running_guests=99), self.policy)
        self.assertEqual(len(v.deficits), 3)

    def test_requirement_derives_from_instance_fields(self) -> None:
        inst = load(self.tmp, """
            arch = "riscv64"
            [machine]
            memory = "2G"
            smp = 4
            [rootfs]
            size_mb = 128
            world_size_mb = 4096
        """)
        need = requirement_for(inst, self.policy)
        self.assertEqual(need.mem_mb, 2048 + 1024)
        self.assertEqual(need.cpus, 4)
        self.assertEqual(need.disk_mb, 4096 + 128)

    def test_disk_floor_applies_when_instance_declares_nothing(self) -> None:
        inst = load(self.tmp, 'arch = "riscv64"\n')
        self.assertEqual(requirement_for(inst, self.policy).disk_mb, 2048)

    def test_smp_defaults_to_one_when_unset(self) -> None:
        inst = load(self.tmp, 'arch = "riscv64"\n')
        self.assertEqual(requirement_for(inst, self.policy).cpus, 1)

    def test_policy_reads_the_environment(self) -> None:
        import os
        saved = {k: os.environ.get(k) for k in
                 ("A20_RESERVE_MEM_MB", "A20_MIN_DISK_MB", "A20_MAX_CONCURRENT",
                  "A20_WAIT_TIMEOUT")}
        try:
            os.environ.update(A20_RESERVE_MEM_MB="4096", A20_MIN_DISK_MB="8192",
                              A20_MAX_CONCURRENT="7", A20_WAIT_TIMEOUT="90")
            p = Policy.from_env()
            self.assertEqual((p.reserve_mem_mb, p.min_disk_mb, p.max_concurrent), (4096, 8192, 7))
            self.assertEqual(p.wait_timeout_s, 90.0)
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def test_unparseable_env_falls_back_to_defaults(self) -> None:
        import os
        saved = os.environ.get("A20_RESERVE_MEM_MB")
        try:
            os.environ["A20_RESERVE_MEM_MB"] = "not-a-number"
            self.assertEqual(Policy.from_env().reserve_mem_mb, DEFAULT_RESERVE_MEM_MB)
        finally:
            if saved is None:
                os.environ.pop("A20_RESERVE_MEM_MB", None)
            else:
                os.environ["A20_RESERVE_MEM_MB"] = saved

    def test_no_wait_fails_instead_of_blocking(self) -> None:
        from a20_resource import preflight
        inst = load(self.tmp, 'arch = "riscv64"\n[machine]\nmemory = "64G"\n')
        with self.assertRaises(SystemExit) as cm:
            preflight(inst, self.policy, self.tmp, wait=False)
        self.assertIn("insufficient host resources", str(cm.exception))

    def test_wait_returns_once_the_host_has_room(self) -> None:
        from unittest.mock import patch
        from a20_resource import preflight
        inst = load(self.tmp, 'arch = "riscv64"\n')
        scarce = self.host(mem_available_mb=10)
        ample = self.host(mem_available_mb=16000)
        with patch("a20_resource.HostResources.snapshot",
                   side_effect=[scarce, scarce, ample]), \
             patch("a20_resource.time.sleep"):
            verdict = preflight(inst, self.policy, self.tmp, wait=True, echo=lambda _m: None)
        self.assertTrue(verdict.ok)


class TestHostMeasurement(unittest.TestCase):
    """The snapshot functions are what the gate actually trusts, so they get
    tested against the real /proc rather than only through injected values."""

    def _meminfo(self) -> dict[str, int]:
        fields = {}
        with open("/proc/meminfo", encoding="ascii") as f:
            for line in f:
                key, _, rest = line.partition(":")
                if key in ("MemTotal", "MemFree", "MemAvailable", "Cached"):
                    fields[key] = int(rest.split()[0]) // 1024
        return fields

    def test_mem_available_is_read_not_free(self) -> None:
        from a20_resource import _mem_available_mb
        info = self._meminfo()
        self.assertEqual(_mem_available_mb(), info["MemAvailable"])

    def test_mem_available_exceeds_free_on_a_cached_host(self) -> None:
        """The property the whole design turns on: on an idle machine the two
        differ by an order of magnitude, and reading `free` would block forever."""
        from a20_resource import _mem_available_mb
        info = self._meminfo()
        self.assertGreaterEqual(_mem_available_mb(), info["MemFree"])
        self.assertGreater(info["MemTotal"] - _mem_available_mb(), 0)

    def test_measurement_is_plausible(self) -> None:
        info = self._meminfo()
        avail = HostResources.snapshot(REPO_ROOT).mem_available_mb
        self.assertGreater(avail, 0)
        self.assertLessEqual(avail, info["MemTotal"])

    def test_guest_count_is_a_nonnegative_int(self) -> None:
        from a20_resource import count_running_guests
        self.assertGreaterEqual(count_running_guests(), 0)

    def test_snapshot_reports_a_usable_cpu_and_disk(self) -> None:
        h = HostResources.snapshot(REPO_ROOT)
        self.assertGreaterEqual(h.cpu_count, 1)
        self.assertGreater(h.disk_free_mb, 0)
        self.assertGreaterEqual(h.load1, 0.0)


class TestSmokeHarnessRobustness(unittest.TestCase):
    """The harness used to swallow the failures that cost the most debugging
    time: a make error reported as "no qemu command found", a Ctrl-C leaving an
    orphaned QEMU, and two runs writing into one log."""

    def test_make_failure_reports_stderr_not_the_dry_run_plan(self) -> None:
        from unittest.mock import patch
        import a20_test
        inst = load_instance("qemu-riscv64")

        class Result:
            returncode = 2
            stdout = "qemu-system-riscv64 -machine virt\n"
            stderr = "Makefile:42: *** missing separator.  Stop.\n"

        with patch("a20_test.subprocess.run", return_value=Result()):
            with self.assertRaises(SystemExit) as cm:
                a20_test._qemu_cmdline(inst)
        msg = str(cm.exception)
        self.assertIn("missing separator", msg)
        self.assertNotIn("machine virt", msg)

    def test_absent_qemu_line_is_distinct_from_make_failure(self) -> None:
        from unittest.mock import patch
        import a20_test
        inst = load_instance("qemu-riscv64")

        class Result:
            returncode = 0
            stdout = "gcc -o kernel.elf kernel/main.c\n"
            stderr = ""

        with patch("a20_test.subprocess.run", return_value=Result()):
            with self.assertRaises(SystemExit) as cm:
                a20_test._qemu_cmdline(inst)
        self.assertIn("no qemu-system command found", str(cm.exception))

    def test_qemu_token_survives_make_and_shell_wrappers(self) -> None:
        from a20_test import _QEMU_TOKEN
        for line, want in (
            ("qemu-system-riscv64 -machine virt", "qemu-system-riscv64"),
            ("make[1]: qemu-system-aarch64 -M virt", "qemu-system-aarch64"),
            ("cd /x && qemu-system-loongarch64 -M virt", "qemu-system-loongarch64"),
            ("/usr/bin/qemu-system-x86_64 -machine q35", "/usr/bin/qemu-system-x86_64"),
        ):
            m = _QEMU_TOKEN.search(line)
            self.assertIsNotNone(m, line)
            self.assertEqual(m.group(1), want)

    def test_qemu_token_does_not_match_unrelated_recipes(self) -> None:
        from a20_test import _QEMU_TOKEN
        for line in ("gcc -o kernel.elf kernel/main.c",
                     "make[1]: Entering directory '/tmp/build'",
                     "echo qemu-system-fake-not-really"):
            self.assertIsNone(_QEMU_TOKEN.search(line), line)

    def test_timeout_parsing(self) -> None:
        from a20_test import _parse_timeout
        self.assertEqual(_parse_timeout("45s"), 45.0)
        self.assertEqual(_parse_timeout(" 1.5s "), 1.5)
        for bad in ("45", "45sec", "", "s", "s45"):
            with self.assertRaises(SystemExit, msg=bad):
                _parse_timeout(bad)

    def test_same_instance_cannot_run_twice(self) -> None:
        from a20_test import _exclusive
        with _exclusive("unit-test-instance"):
            with self.assertRaises(SystemExit) as cm:
                with _exclusive("unit-test-instance"):
                    pass
        self.assertIn("already running", str(cm.exception))

    def test_lock_is_released_when_the_holder_exits(self) -> None:
        """flock is released by the kernel on process death, so a killed run
        cannot leave an instance permanently unusable."""
        import subprocess as sp
        from a20_test import _exclusive
        code = ("import sys; sys.path.insert(0, 'tools');"
                "from a20_test import _exclusive\n"
                "with _exclusive('unit-test-reclaim'):\n"
                "    print('held', flush=True)\n")
        first = sp.Popen([sys.executable, "-c", code], stdout=sp.PIPE, text=True)
        self.assertEqual(first.stdout.readline().strip(), "held")
        first.kill()
        first.wait()
        with _exclusive("unit-test-reclaim"):
            pass  # reacquired, so the lock really was released

    def test_reap_kills_the_whole_process_group(self) -> None:
        """A plain kill() leaves whatever QEMU spawned running.  The child here
        ignores SIGTERM and spawns a grandchild that also ignores it, so only a
        process-group signal plus SIGKILL escalation can clear both."""
        import os
        import subprocess as sp
        import time
        from a20_test import _reap
        inner = ("import signal,subprocess,sys,time\n"
                 "k=subprocess.Popen([sys.executable,'-c',"
                 "'import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);"
                 "time.sleep(300)'])\n"
                 "print(k.pid,flush=True)\n"
                 "signal.signal(signal.SIG_IGN if False else signal.SIGTERM,"
                 "signal.SIG_IGN)\n"
                 "time.sleep(300)\n")
        proc = sp.Popen([sys.executable, "-c", inner], stdout=sp.PIPE,
                        start_new_session=True, text=True)
        grandchild = int(proc.stdout.readline().strip())
        try:
            _reap(proc)
            self.assertIsNotNone(proc.poll())
            time.sleep(0.3)
            for pid in (proc.pid, grandchild):
                with self.assertRaises(ProcessLookupError, msg=f"pid {pid} leaked"):
                    os.kill(pid, 0)
        finally:
            for pid in (proc.pid, grandchild):
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass


class TestCliArgumentHandling(unittest.TestCase):
    """A mistyped a20 flag used to be forwarded to make, where it silently
    became a variable override or a goal -- the build changed and nothing said
    so.  Only an explicit "--" may pass arguments through.

    Every test here stubs the action boundary.  Without that, a regression in
    the argument gate does not fail the test -- it lets the call through to a
    real QEMU boot, and the suite hangs instead of reporting.
    """

    def cli(self):
        """Load tools/a20 as a module with every action replaced by a stub."""
        import contextlib
        import importlib.machinery
        import importlib.util
        import io
        from unittest.mock import patch
        loader = importlib.machinery.SourceFileLoader(f"a20_cli_{id(self)}", str(REPO_ROOT / "tools" / "a20"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        mod = importlib.util.module_from_spec(spec)
        loader.exec_module(mod)

        def stub(*_a, **_k):
            raise AssertionError("an action ran; the argument gate let it through")

        patches = [patch.object(mod, name, stub) for name in
                   ("exec_make", "build_instance", "run_test", "run_flash", "run_package")]
        for ctx in patches:
            ctx.start()
            self.addCleanup(ctx.stop)
        return mod, contextlib.nullcontext()

    def run_main(self, argv: list[str]) -> int:
        mod, _ = self.cli()
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            return mod.main(argv)

    def test_typo_is_an_error_not_a_forwarded_flag(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            self.run_main(["run", "qemu-riscv64", "--dry-rnu"])
        self.assertEqual(cm.exception.code, 2)

    def test_typo_after_a_valid_flag_is_still_caught(self) -> None:
        with self.assertRaises(SystemExit):
            self.run_main(["run", "qemu-riscv64", "--dry-run", "--wait-timout", "5"])

    def test_explicit_double_dash_forwards_to_make(self) -> None:
        import contextlib
        import io
        mod, _ = self.cli()
        seen = {}

        def record(_inst, extra, _dry):
            seen["extra"] = extra
            return 0

        mod.build_instance = record
        with contextlib.redirect_stdout(io.StringIO()):
            rc = mod.main(["build", "qemu-riscv64", "--dry-run", "--", "-j8"])
        self.assertEqual(rc, 0)
        self.assertEqual(seen["extra"], ["-j8"])

    def test_show_vars_prints_without_running_anything(self) -> None:
        import contextlib
        import io
        mod, _ = self.cli()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(mod.main(["show-vars", "qemu-riscv64"]), 0)
        self.assertIn("ARCH=riscv64", buf.getvalue())

    def test_list_reports_success_on_a_clean_tree(self) -> None:
        self.assertEqual(self.run_main(["list"]), 0)

    def test_list_fails_when_an_instance_is_invalid(self) -> None:
        """`a20 list` always returned 0, so it could not be used as a gate."""
        import contextlib
        import io
        from unittest.mock import patch
        mod, _ = self.cli()
        broken = Path(self.id().replace(".", "_") + ".toml")
        broken.write_text('arch = "riscv64"\nname = "broken"\n', encoding="utf-8")
        self.addCleanup(broken.unlink)
        with patch.object(mod, "INSTANCES_DIR", broken.parent), \
             patch.object(mod, "validate_instance", return_value=["synthetic failure"]), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mod.main(["list"]), 1)
