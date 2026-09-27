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
from a20_registry import (  # noqa: E402
    RegistryError,
    load_flash_backends,
    load_registry,
    validate_flash_backends,
    validate_registry,
)
from a20_validate import validate_instance  # noqa: E402


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
