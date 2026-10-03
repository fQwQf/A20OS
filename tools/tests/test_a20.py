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

import json
import os
import pty
import shutil
import subprocess
import sys
import tempfile
import time
import textwrap
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from a20_error import A20Error, ToolError
from a20_board import FLASH_TARGET_REACHABLE, run_flash  # noqa: E402
from a20_derive import derive_make_vars  # noqa: E402
from a20_instance import InstanceError, parse_instance, section_is_set  # noqa: E402
from a20_make import REPO_ROOT  # noqa: E402
from a20_console import (  # noqa: E402
    ConsoleError,
    SerialTransport,
    run_console_session,
)
from a20_manifest import Artifact, GitState  # noqa: E402
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


def _alive(fd: int) -> bool:
    try:
        os.fstat(fd)
        return True
    except OSError:
        return False


def load_instance(name: str):
    return parse_instance(REPO_ROOT / "instances" / f"{name}.toml")


def load(tmp: Path, text: str):
    path = tmp / "case.toml"
    path.write_text(textwrap.dedent(text).lstrip(), encoding="utf-8")
    return parse_instance(path)


def derived(tmp: Path, text: str) -> dict[str, str]:
    return dict(v.split("=", 1) for v in derive_make_vars(load(tmp, text)))


def _ctx(fn):
    """A context manager that runs fn() on enter, for stubbing _exclusive."""
    import contextlib

    @contextlib.contextmanager
    def _cm():
        fn()
        yield
    return _cm()


def _load_cli():
    """Import tools/a20 (a script, not a module) so its functions can be called."""
    import importlib.machinery
    import importlib.util
    loader = importlib.machinery.SourceFileLoader("a20_cli_helper", str(REPO_ROOT / "tools" / "a20"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def _pump(proc, log: Path, stop) -> None:
    """Copy a child's output into `log` so _watch can observe it growing."""
    with log.open("wb") as sink:
        for line in proc.stdout:
            sink.write(line)
            sink.flush()
            if stop.is_set():
                return


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
        """)
        self.assertEqual(got["FAT32_IMAGE_MB"], "256")
        self.assertEqual(got["EXT4_IMAGE_MB"], "64")
        # rootfs.extra_size_mb and rootfs.extra_packages are gone with the
        # source-built extra disk; the second ext4 slot is an apk world image
        # sized by rootfs.world_size_mb (PKG_SIZE_MB).
        self.assertNotIn("EXTRA_IMAGE_MB", got)
        self.assertNotIn("EXTRA_PACKAGES", got)

    def test_a20_consumed_fields_emit_no_variables(self) -> None:
        """gui.enabled, test.commands/expect are a20's own, not make's.

        machine.extra_qemu is deliberately absent from the forbidden list: the
        GPU work made it a first-class knob, so a20 now hands it to make as
        EXTRA_QEMU and the Makefile folds it into QEMU_FLAGS.  That is what lets
        the 3D device reach QEMU at all.  What must stay un-emitted is the
        a20-only bookkeeping: the GUI on/off switch and the serial script.
        """
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
        for absent in ("QEMU_GUI", "SMOKE_COMMANDS", "SMOKE_EXPECT"):
            self.assertNotIn(absent, got)
        # Routed to make, exactly once, joined the way the Makefile expects.
        self.assertEqual(got["EXTRA_QEMU"], "-device riscv-iommu-pci,bus=pcie.0")


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

    def test_retired_fit_sdcard_variant_is_rejected(self) -> None:
        """`extra` resolved to `make vf2-extra`, now a deprecation stub.

        Accepting it would let `a20 package` pass validation and only fail
        later inside make, which is the worse failure mode.
        """
        errors = self.check('arch = "riscv64"\nboard = "visionfive2"\n'
                            'abi = "both"\n[package]\nkind = "fit-sdcard"\n'
                            'variant = "extra"\n')
        self.assertTrue(any("package.variant" in e for e in errors), errors)
        self.assertEqual(self.check('arch = "riscv64"\nboard = "visionfive2"\n'
                                    'abi = "both"\n[package]\nkind = "fit-sdcard"\n'
                                    'variant = "sdcard"\n'), [])


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
        with self.assertRaises(A20Error) as cm:
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
        with self.assertRaises(A20Error) as cm:
            self.flash('arch = "riscv64"\nboard = "visionfive2"\n[flash]\ntool = "openocd"\n')
        self.assertIn("not validated for board", str(cm.exception))
        self.build_m.assert_not_called()

    def test_unregistered_backend_is_refused(self) -> None:
        with self.assertRaises(A20Error) as cm:
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
        # A failing build raises rather than returning make's status, so a
        # failed build can never be mistaken for a programming success.
        self.build_m.side_effect = ToolError("make kernel-only failed", status=2)
        with self.assertRaises(ToolError):
            self.flash("""
                arch = "armv7m"
                [stm32]
                flash_kb = 512
                ram_kb = 64
                [flash]
                tool = "openocd"
            """)
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

    def test_boot_media_names_a_path_its_package_kind_produces(self) -> None:
        """`a20 deploy` writes boot_media, so the image it names has to exist.

        cmd_deploy calls run_package before target-write-media, so an instance
        that declares a [package].kind gets its artifact built.  An instance that
        names boot_media without one would instead build a kernel into the build
        directory and then fail at the dd step, because nothing ever produced the
        image it committed to.  This is that failure, asserted against the real
        manifests so it cannot come back one manifest at a time.
        """
        failures: list[str] = []
        for path in sorted((REPO_ROOT / "instances").glob("*.toml")):
            inst = parse_instance(path)
            if not inst.target.boot_media:
                continue
            if not inst.target.media_device:
                continue          # separately covered: deploy refuses this
            if not inst.package.kind:
                failures.append(f"{path.name}: boot_media with no [package].kind")
            elif not inst.target.boot_media[0].startswith("build/"):
                failures.append(
                    f"{path.name}: boot_media {inst.target.boot_media[0]} is not "
                    f"under build/, so no make target stages it there")
        self.assertEqual(failures, [], "\n".join(failures))


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
        with self.assertRaises(A20Error) as cm:
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
            with self.assertRaises(A20Error) as cm:
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
            with self.assertRaises(A20Error) as cm:
                a20_test._qemu_cmdline(inst)
        self.assertIn("no qemu-system command", str(cm.exception))

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
            with self.assertRaises(A20Error) as cm:
                with _exclusive("unit-test-instance"):
                    pass
        self.assertIn("already running", str(cm.exception))

    def test_lock_is_released_when_the_holder_exits(self) -> None:
        """flock is released by the kernel on process death, so a killed run
        cannot leave an instance permanently unusable."""
        import subprocess as sp
        from a20_test import _exclusive
        code = (f"import sys; sys.path.insert(0, {str(REPO_ROOT / 'tools')!r});"
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
        # argparse's own exit(2) is the usage-error convention, so this stays
        # a SystemExit rather than an A20Error.
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

    def test_show_reports_actions_ports_and_media_in_one_screen(self) -> None:
        """`show` exists so the choice between 46 instances needs one command."""
        import contextlib
        import io
        mod, _ = self.cli()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(mod.main(["show", "vf2-physical"]), 0)
        out = buf.getvalue()
        self.assertIn("console", out)          # the action
        self.assertIn("/dev/ttyUSB0", out)     # where it talks to the board
        self.assertIn("/dev/sda", out)         # where it would write media
        self.assertIn("manifest", out)         # so the file can be opened

    def test_show_does_not_claim_a_command_that_would_refuse(self) -> None:
        import contextlib
        import io
        mod, _ = self.cli()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(mod.main(["show", "stm32f103"]), 0)
        out = buf.getvalue()
        # stm32f103 has no [stm32] qemu flag, so `run` must not be advertised.
        self.assertNotIn("run", out.split("actions")[1].splitlines()[0])

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


class TestApplicableActions(unittest.TestCase):
    """The `list` capability column must not promise what a command refuses."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def acts(self, text: str):
        from a20_instance import applicable_actions
        return applicable_actions(load(self.tmp, text))

    def test_a_plain_qemu_instance_runs_but_is_not_a_smoke(self) -> None:
        acts = self.acts("""
            arch = "riscv64"
            board = "qemu-virt-riscv64"
        """)
        self.assertEqual(acts, ("build", "run", "debug"))

    def test_expect_is_what_makes_a_smoke_runnable(self) -> None:
        base = """
            arch = "riscv64"
            board = "qemu-virt-riscv64"
            [test]
        """
        # run_test rejects [test] without expect, so it must not advertise it.
        self.assertNotIn("test", self.acts(base + 'timeout = "20s"\n'))
        self.assertIn("test", self.acts(base + 'expect = ["PASS"]\n'))

    def test_armv7m_needs_the_stm32_qemu_flag_to_run(self) -> None:
        base = """
            arch = "armv7m"
            [stm32]
            flash_kb = 64
        """
        self.assertNotIn("run", self.acts(base))
        self.assertIn("run", self.acts(base + "qemu = true\n"))

    def test_target_section_unlocks_console_but_not_bare_deploy(self) -> None:
        """A [target] section alone buys console, not deploy.

        cmd_deploy has to put something on the board: either a writable medium or
        a boot chain to hand the kernel to.  Advertising deploy for a target that
        has neither is what left the storage-less SBC ports with a command that
        built the kernel and then attached to a board which never received it.
        This expectation used to assert the opposite, and disagreed with
        test_deploy_is_withheld_when_media_has_nowhere_to_go directly below.
        """
        self.assertNotIn("console", self.acts('arch = "riscv64"\n'))
        acts = self.acts("""
            arch = "riscv64"
            [target]
            serial = "/dev/ttyUSB0"
        """)
        self.assertIn("console", acts)
        self.assertNotIn("deploy", acts)

    def test_deploy_is_advertised_for_a_handoff_board(self) -> None:
        """A board with no block driver still deploys: via [handoff]."""
        acts = self.acts("""
            arch = "riscv64"
            [target]
            serial = "/dev/ttyUSB0"
            [handoff]
            method = "tftp"
            commands = ["tftp ${loadaddr} kernel.bin"]
        """)
        self.assertIn("deploy", acts)

    def test_deploy_is_withheld_when_media_has_nowhere_to_go(self) -> None:
        """cmd_deploy refuses boot_media without media_device."""
        acts = self.acts("""
            arch = "riscv64"
            [target]
            serial = "/dev/ttyUSB0"
            boot_media = ["build/a.img"]
        """)
        self.assertIn("console", acts)
        self.assertNotIn("deploy", acts)

    def test_flash_and_package_follow_their_required_fields(self) -> None:
        self.assertNotIn("flash", self.acts('arch = "riscv64"\n[flash]\n'))
        self.assertIn("flash", self.acts(
            'arch = "riscv64"\n[flash]\ntool = "openocd"\n'))
        self.assertIn("package", self.acts(
            'arch = "x86_64"\n[package]\nkind = "grub-iso"\n'))


class TestOperatorErrorContract(unittest.TestCase):
    """Failures are one line on stderr with a documented exit code.

    Everything here used to escape as a raw Python exception: a make query the
    Makefile rejected printed a five-frame MakeQueryError traceback, a missing
    `make` printed FileNotFoundError, and validation failures were printed to
    stdout, so `2>/dev/null` did not filter them and a redirected report still
    carried the failures.
    """

    def cli(self):
        import importlib.machinery
        import importlib.util
        loader = importlib.machinery.SourceFileLoader(
            f"a20_errcli_{id(self)}", str(REPO_ROOT / "tools" / "a20"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        mod = importlib.util.module_from_spec(spec)
        loader.exec_module(mod)
        return mod

    def test_every_gated_command_accepts_the_resource_flags(self) -> None:
        """build/flash/package are gated too, so they need the waiting flags.

        Only run/debug/test had them, so the other three raised AttributeError
        on `args.no_wait` and died with a traceback before doing any work --
        `a20 flash` and `a20 package` were unusable. argparse turns an unknown
        flag into SystemExit, so accepting these is the observable difference.
        """
        import contextlib
        import io
        mod = self.cli()
        from a20_error import A20Error
        for name in ("build", "run", "debug", "test", "flash", "package"):
            for flag in ("--no-wait", "--wait-timeout"):
                argv = [name, "no-such-instance-xyz"] + (
                    [flag, "1"] if flag == "--wait-timeout" else [flag])
                with contextlib.redirect_stdout(io.StringIO()), \
                        contextlib.redirect_stderr(io.StringIO()):
                    try:
                        mod.main(argv)
                    except A20Error:
                        pass          # the flag parsed; the loader said no
                    except SystemExit as e:
                        self.fail(f"{name} rejected {flag} (usage error {e.code})")

    def test_gating_tolerates_a_namespace_without_the_wait_flags(self) -> None:
        """A missing flag must not surface as an AttributeError."""
        import argparse
        mod = self.cli()
        mod._gate_resources(load_instance("qemu-riscv64"), argparse.Namespace(),
                           guest=False)

    def test_flash_and_package_report_their_own_missing_section(self) -> None:
        import contextlib
        import io
        from a20_error import A20Error
        mod = self.cli()
        for cmd, needle in (("flash", "[flash].tool"), ("package", "[package] kind")):
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(A20Error, msg=cmd) as cm:
                    mod.main([cmd, "qemu-riscv64"])
            self.assertIn(needle, str(cm.exception), cmd)

    def test_exit_codes_are_disjoint(self) -> None:
        from a20_error import EXIT_FAIL, EXIT_OK, EXIT_TIMEOUT, EXIT_TOOL, EXIT_USAGE
        codes = [EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_TOOL, EXIT_TIMEOUT]
        self.assertEqual(len(set(codes)), len(codes),
                         "exit codes must be distinguishable by a script")

    def test_missing_instance_suggests_the_nearest_real_name(self) -> None:
        mod = self.cli()
        with self.assertRaises(A20Error) as cm:
            mod._resolve("qemu-riscv6")
        self.assertIn("did you mean", cm.exception.hint or "")
        self.assertIn("qemu-riscv64", cm.exception.hint or "")

    def test_missing_instance_without_a_near_miss_points_at_list(self) -> None:
        mod = self.cli()
        with self.assertRaises(A20Error) as cm:
            mod._resolve("zzz-nothing-like-this")
        self.assertIn("a20 list", cm.exception.hint or "")

    def test_report_writes_to_stderr_and_includes_the_hint(self) -> None:
        import contextlib
        import io
        from a20_error import EXIT_FAIL, report
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = report(A20Error("it broke", hint="try this"))
        self.assertEqual(err.getvalue(), "error: it broke\nhint: try this\n")
        self.assertEqual(code, EXIT_FAIL)

    def test_tool_status_does_not_masquerade_as_a_usage_error(self) -> None:
        from a20_error import EXIT_TIMEOUT, EXIT_TOOL, EXIT_USAGE, ToolError
        # make exits 2 for a missing rule; a20 reserves 2 for its own usage errors.
        self.assertEqual(ToolError("make dev-build failed", status=2).exit_code_for(),
                         EXIT_TOOL)
        self.assertNotEqual(ToolError("x", status=2).exit_code_for(), EXIT_USAGE)

    def test_timeout_keeps_its_own_code(self) -> None:
        from a20_error import EXIT_TIMEOUT, ToolError
        self.assertEqual(ToolError("qemu timed out", status=124).exit_code_for(),
                         EXIT_TIMEOUT)

    def test_exec_make_raises_instead_of_returning_make_status(self) -> None:
        from a20_error import ToolError
        from a20_make import exec_make
        with patch("a20_make.subprocess.run") as run:
            run.return_value = SimpleNamespace(returncode=2)
            with self.assertRaises(ToolError) as cm:
                exec_make(load_instance("qemu-riscv64"), "dev-build", [], False)
        self.assertEqual(cm.exception.status, 2)

    def test_exec_make_reports_a_missing_make_distinctly(self) -> None:
        from a20_error import ToolError
        from a20_make import exec_make
        with patch("a20_make.subprocess.run",
                   side_effect=FileNotFoundError(2, "No such file", "make")):
            with self.assertRaises(ToolError) as cm:
                exec_make(load_instance("qemu-riscv64"), "dev-build", [], False)
        self.assertIn("make", str(cm.exception))
        self.assertIn("PATH", cm.exception.hint or "")

    def test_schema_violation_is_a_clean_error_not_a_traceback(self) -> None:
        # parse_instance is the schema boundary: an unknown key is refused there.
        with tempfile.TemporaryDirectory() as td:
            bad = Path(td) / "bad.toml"
            bad.write_text('arch = "riscv64"\n[gui]\nenabled = true\n'
                           'frame_window = 15\n', encoding="utf-8")
            with self.assertRaises(InstanceError) as cm:
                parse_instance(bad)
        self.assertIn("unknown key", str(cm.exception))

    def test_semantic_failures_are_not_written_to_stdout(self) -> None:
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as td:
            # smp > 1 on a board that is not verified for SMP is a semantic
            # error, so it survives parsing and is caught by validate_instance.
            bad = Path(td) / "bad.toml"
            bad.write_text('arch = "riscv64"\nboard = "visionfive2"\n'
                           '[machine]\nsmp = 8\n', encoding="utf-8")
            inst = parse_instance(bad)
            errors = validate_instance(inst, REPO_ROOT)
        self.assertTrue(errors, "smp on an unverified board must be rejected")
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            print("FAIL bad.toml: " + "; ".join(errors), file=sys.stderr)
        self.assertEqual(out.getvalue(), "")
        self.assertTrue(err.getvalue().startswith("FAIL bad.toml:"))


class TestSmokeProgressAndLifecycle(unittest.TestCase):
    """A smoke used to be silent for its whole timeout, then print one line.

    QEMU's output only ever went to the log file, so a boot that took four
    minutes to reach its first marker looked exactly like a hang.  These pin the
    progress reporting and the two lifecycle defects that came with it.
    """

    def _child(self, script: str):
        return subprocess.Popen([sys.executable, "-c", script],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_watch_announces_each_marker_as_it_appears(self) -> None:
        import tempfile
        import threading
        from a20_test import _watch
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "boot.log"
            proc = self._child(
                "import time\n"
                "print('MARK_A', flush=True)\n"
                "time.sleep(0.5)\n"
                "print('MARK_B', flush=True)\n"
                "time.sleep(0.3)\n")
            stop = threading.Event()
            t = threading.Thread(target=_pump, args=(proc, log, stop), daemon=True)
            t.start()
            seen = []
            timed_out = _watch(proc, log, ("MARK_A", "MARK_B"), 20.0, seen.append)
            stop.set()
            t.join(timeout=2)
            proc.wait()
        self.assertFalse(timed_out)
        self.assertEqual(len(seen), 2, f"both markers should be announced, got {seen}")
        self.assertTrue(any("MARK_A" in s for s in seen))
        self.assertTrue(any("MARK_B" in s for s in seen))

    def test_watch_reads_the_log_one_last_time_when_the_guest_exits(self) -> None:
        """A marker landing between the final poll and the exit is not a failure.

        The guest can write its last marker and exit inside one poll interval.
        _watch used to return the moment poll() reported the exit, without
        reading the log again, so that marker was never seen and a healthy boot
        was reported as a missing one.
        """
        import tempfile
        from unittest.mock import patch
        from a20_test import _watch

        class FakeProc:
            calls = 0

            def poll(self):
                FakeProc.calls += 1
                return None if FakeProc.calls == 1 else 0

        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "boot.log"
            log.write_bytes(b"early boot output\n")
            seen = []

            def land_marker(_seconds):
                log.write_bytes(b"early boot output\nFINAL_MARKER\n")

            with patch("a20_test.time.sleep", side_effect=land_marker):
                timed_out = _watch(FakeProc(), log, ("FINAL_MARKER",), 20.0, seen.append)
        self.assertFalse(timed_out)
        self.assertEqual(len(seen), 1, f"expected one announcement, got {seen}")
        self.assertIn("FINAL_MARKER", seen[0])

    def test_watch_reports_a_timeout_rather_than_hanging(self) -> None:
        import tempfile
        from a20_test import _watch
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "quiet.log"
            proc = self._child("import time\ntime.sleep(30)\n")
            seen = []
            timed_out = _watch(proc, log, ("NEVER",), 0.6, seen.append)
            proc.kill()
            proc.wait()
        self.assertTrue(timed_out)
        self.assertEqual(seen, [], "no marker appeared, so nothing should be announced")

    def test_the_lock_is_taken_before_the_build(self) -> None:
        """Two runs of one instance must not both build into the same BUILD_DIR.

        The lock used to be taken after build_instance, so both runs did the
        full multi-minute build and only then did the loser find out.
        """
        import a20_test
        order = []
        with patch("a20_test.build_instance",
                   side_effect=lambda *a, **k: order.append("build")), \
             patch("a20_test._qemu_cmdline", return_value=["true"]), \
             patch("a20_test._exclusive",
                   return_value=_ctx(lambda: order.append("lock"))), \
             patch("a20_test.preflight"), \
             patch("a20_test.SMOKE_LOG_DIR", Path(tempfile.mkdtemp())), \
             patch("a20_test.subprocess.Popen"), \
             patch("a20_test._watch", return_value=True), \
             patch("a20_test._reap"):
            a20_test.run_test(load_instance("smoke-riscv64"), [], True)
        self.assertEqual(order[:2], ["lock", "build"],
                         f"lock must precede the build, got {order}")

    def test_the_guest_is_reaped_even_when_waiting_raises(self) -> None:
        import a20_test
        reaped = []
        with patch("a20_test.build_instance"), \
             patch("a20_test._qemu_cmdline", return_value=["true"]), \
             patch("a20_test._exclusive", return_value=_ctx(lambda: None)), \
             patch("a20_test.preflight"), \
             patch("a20_test.SMOKE_LOG_DIR", Path(tempfile.mkdtemp())), \
             patch("a20_test.subprocess.Popen") as popen, \
             patch("a20_test._watch", side_effect=RuntimeError("boom")), \
             patch("a20_test._reap", side_effect=lambda p: reaped.append(p)):
            popen.return_value = SimpleNamespace(
                poll=lambda: None, returncode=None,
                stdin=SimpleNamespace(close=lambda: None))
            with self.assertRaises(RuntimeError):
                a20_test.run_test(load_instance("smoke-riscv64"), [], False)
        self.assertEqual(len(reaped), 1,
                         "a guest must be reaped on every exit path, not only on timeout")

    def test_console_dry_run_does_not_open_the_serial_port(self) -> None:
        mod = _load_cli()
        args = SimpleNamespace(instance="vf2-physical", no_reset=False,
                               no_flash=False, dry_run=True, make_args=[],
                               wait_timeout=None, no_wait=False)
        opened = []
        with patch("a20_console.open_transport",
                   side_effect=AssertionError("must not open a port on --dry-run")), \
             patch.object(mod, "_load", return_value=load_instance("vf2-physical")):
            import contextlib, io
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = mod.cmd_console(args)
        self.assertEqual(rc, 0)
        self.assertIn("would open /dev/ttyUSB0", buf.getvalue())

    def test_build_scope_does_not_block_on_guest_memory(self) -> None:
        from a20_resource import HostResources, Policy, Requirement, evaluate
        have = HostResources(
            mem_available_mb=256, cpu_count=8, load1=0.0,
            disk_free_mb=99_999, running_guests=0, busy_ports=())
        need = Requirement(mem_mb=4096, cpus=1, disk_mb=1024, guests=0)
        pol = Policy()
        self.assertFalse(evaluate(need, have, pol, guest=True).ok,
                         "a guest run must be refused")
        self.assertTrue(evaluate(need, have, pol, guest=False).ok,
                        "a build-only step must not be blocked by guest memory")

    def test_every_deficit_carries_a_remedy(self) -> None:
        from a20_resource import HostResources, Policy, Requirement, evaluate
        have = HostResources(
            mem_available_mb=256, cpu_count=1, load1=1.0,
            disk_free_mb=10, running_guests=4, busy_ports=(("127.0.0.1", 2222),))
        v = evaluate(Requirement(mem_mb=4096, cpus=8, disk_mb=8192, guests=1,
                                 ports=(("127.0.0.1", 2222),)),
                     have, Policy())
        self.assertFalse(v.ok)
        self.assertEqual(len(v.remedies), len(v.deficits),
                         "every deficit must say what would clear it")
        self.assertIn("A20_MAX_CONCURRENT", v.remedy())


class TestMakeQuery(unittest.TestCase):
    """Artifact paths must come from make, never from a Python copy of the
    BUILD_DIR formula -- that name encodes a dozen build switches."""

    def test_returns_one_value_per_requested_name(self) -> None:
        from a20_make import query_make
        got = query_make(load_instance("qemu-riscv64"),
                         ["BUILD_DIR", "KERNEL_ELF", "KERNEL_BIN"])
        self.assertEqual(len(got), 3)
        self.assertTrue(got["BUILD_DIR"].startswith(".kernel-build/riscv64-qemu-virt-riscv64-"))
        self.assertTrue(got["KERNEL_ELF"].endswith("kernel.elf"))

    def test_derived_instance_variables_reach_make(self) -> None:
        from a20_make import query_make
        got = query_make(load_instance("qemu-riscv64-smp4"), ["BUILD_DIR"])
        self.assertIn("-smp4", got["BUILD_DIR"])

    def test_no_names_is_a_no_op(self) -> None:
        from a20_make import query_make
        self.assertEqual(query_make(load_instance("qemu-riscv64"), []), {})

    def test_undefined_variable_yields_empty_not_an_error(self) -> None:
        """make expands an undefined variable to "" and exits 0, so a typo in a
        requested name is indistinguishable from a legitimately empty one.  Pinned
        so nobody relies on it raising."""
        from a20_make import query_make
        self.assertEqual(query_make(load_instance("qemu-riscv64"), ["NO_SUCH_VARIABLE_XYZ"]),
                         {"NO_SUCH_VARIABLE_XYZ": ""})


class TestArtifactLedger(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_sha256_matches_hashlib(self) -> None:
        import hashlib
        from a20_manifest import sha256_file
        blob = os.urandom(3 * 1024 * 1024 + 17)  # spans several chunks
        f = self.tmp / "blob.bin"
        f.write_bytes(blob)
        self.assertEqual(sha256_file(f), hashlib.sha256(blob).hexdigest())

    def test_human_sizes(self) -> None:
        from a20_manifest import _human
        self.assertEqual(_human(512), "512 B")
        self.assertEqual(_human(2048), "2.0 KiB")
        self.assertEqual(_human(4 * 1024 * 1024), "4.0 MiB")

    def test_bringup_instance_offers_no_rootfs_images(self) -> None:
        from a20_manifest import _candidates
        roles = [r for r, _p in _candidates(load_instance("qemu-riscv64-bringup"),
                                            _vars_for("qemu-riscv64-bringup"))]
        self.assertNotIn("rootfs-fat32", roles)
        self.assertIn("kernel-elf", roles)

    @unittest.skipUnless(shutil.which("arm-none-eabi-gcc"),
                         "arm-none-eabi toolchain absent; make refuses to resolve MCU paths")
    def test_mcu_instance_offers_no_rootfs_images(self) -> None:
        from a20_manifest import _candidates
        roles = [r for r, _p in _candidates(load_instance("stm32f103-xuanwu"),
                                            _vars_for("stm32f103-xuanwu"))]
        self.assertNotIn("rootfs-fat32", roles)

    def test_dev_instance_offers_rootfs_images(self) -> None:
        from a20_manifest import _candidates
        roles = [r for r, _p in _candidates(load_instance("qemu-riscv64"),
                                            _vars_for("qemu-riscv64"))]
        self.assertIn("rootfs-fat32", roles)
        self.assertIn("rootfs-ext4", roles)

    def test_visionfive2_offers_the_boot_chain(self) -> None:
        from a20_manifest import _candidates
        roles = [r for r, _p in _candidates(load_instance("vf2-sdcard"),
                                            _vars_for("vf2-sdcard"))]
        for role in ("opensbi-fw_dynamic", "u-boot-itb", "fit-image", "sd-card"):
            self.assertIn(role, roles)

    def test_absent_artifacts_are_reported_not_invented(self) -> None:
        from a20_manifest import _artifact
        got = _artifact(self.tmp, "kernel-elf", "nope/kernel.elf")
        self.assertIsInstance(got, str)
        self.assertEqual(got, "nope/kernel.elf")

    def test_present_artifact_carries_size_and_hash(self) -> None:
        from a20_manifest import _artifact
        f = self.tmp / "k.elf"
        f.write_bytes(b"x" * 100)
        got = _artifact(self.tmp, "kernel-elf", "k.elf")
        self.assertIsInstance(got, Artifact)
        self.assertEqual(got.size, 100)
        self.assertEqual(len(got.sha256), 64)

    def test_json_is_parseable_and_sorted(self) -> None:
        from a20_manifest import Manifest, render_json
        m = _manifest()
        parsed = json.loads(render_json(m))
        self.assertEqual(parsed["instance"], "qemu-riscv64")
        self.assertEqual(parsed["git"]["head"], "deadbeef")
        self.assertEqual(parsed["artifacts"][0]["size"], 10)

    def test_markdown_is_a_pasteable_block(self) -> None:
        from a20_manifest import render_markdown
        text = render_markdown(_manifest())
        self.assertTrue(text.startswith("<!-- generated by:"))
        self.assertIn("| artifact | size | sha256 |", text)
        self.assertIn("`abc123`", text)

    def test_table_states_when_nothing_is_built(self) -> None:
        from a20_manifest import Manifest, render_table
        text = render_table(Manifest(instance="x", arch="riscv64", board="b", abi=None,
                                     build_dir="d", git=GitState("h", "br", False),
                                     variables={}, artifacts=(), missing=("a/b",)))
        self.assertIn("no artifacts present", text)
        self.assertIn("a/b", text)

    def test_paths_are_repository_relative(self) -> None:
        """A ledger full of one contributor's home paths is not portable, which
        is how the hand-copied board records became unverifiable."""
        from a20_manifest import collect
        names = ["qemu-riscv64", "vf2-sdcard", "qemu-x86_64", "release-riscv64"]
        if shutil.which("arm-none-eabi-gcc"):
            names.append("stm32f103-xuanwu")
        for inst_name in names:
            for a in collect(load_instance(inst_name)).artifacts:
                self.assertFalse(a.path.startswith("/"), a.path)
                self.assertNotIn("/home/", a.path)

    @unittest.skipIf(shutil.which("arm-none-eabi-gcc"),
                     "toolchain present, so the ledger resolves MCU paths normally")
    def test_missing_cross_toolchain_is_reported_not_faked(self) -> None:
        """A ledger that silently omitted the artifacts it could not resolve
        would be worse than useless on a board. make refuses to evaluate the
        Makefile without the cross toolchain, and that refusal must surface."""
        from a20_make import MakeQueryError
        from a20_manifest import collect
        with self.assertRaises(MakeQueryError) as cm:
            collect(load_instance("stm32f103-xuanwu"))
        self.assertIn("arm-none-eabi", str(cm.exception))

    def test_git_state_reports_head_and_dirtiness(self) -> None:
        from a20_manifest import git_state
        g = git_state(REPO_ROOT)
        self.assertTrue(g.head)
        self.assertIsInstance(g.dirty, bool)


def _vars_for(instance_name: str) -> dict[str, str]:
    from a20_make import query_make
    return query_make(load_instance(instance_name),
                      ("BUILD_DIR", "KERNEL_ELF", "KERNEL_BIN", "FAT32_IMG",
                       "EXT4_IMG", "PKG_IMAGE_DIR", "PKG_ARCH"))


def _manifest():
    from a20_manifest import Artifact, GitState, Manifest
    return Manifest(
        instance="qemu-riscv64", arch="riscv64", board="qemu-virt-riscv64", abi="both",
        build_dir=".kernel-build/x", git=GitState("deadbeef", "br", True),
        variables={"BUILD_DIR": ".kernel-build/x"},
        artifacts=(Artifact("kernel-elf", ".kernel-build/x/kernel.elf", 10, "abc123"),),
    )


class TestTargetSection(unittest.TestCase):
    """[target] is the first non-QEMU-shaped section in the schema: it describes
    the board on the other side of the serial cable."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def check(self, text: str) -> list[str]:
        return validate_instance(load(self.tmp, text), REPO_ROOT)

    BASE = """
        arch = "riscv64"
        board = "visionfive2"
        [target]
        serial = "/dev/ttyUSB0"
        console_check = ["System ready"]
    """

    def test_minimal_target_validates(self) -> None:
        self.assertEqual(self.check(self.BASE), [])

    def test_absent_section_is_not_an_error(self) -> None:
        self.assertEqual(self.check('arch = "riscv64"\n'), [])

    def test_section_is_set_detects_it(self) -> None:
        inst = load(self.tmp, self.BASE)
        self.assertTrue(section_is_set(inst.target))
        self.assertFalse(section_is_set(load(self.tmp, 'arch = "riscv64"\n').target))

    def test_serial_is_required(self) -> None:
        errs = self.check('arch = "riscv64"\nboard = "visionfive2"\n[target]\nbaud = 115200\n')
        self.assertTrue(any("target.serial" in e for e in errs), errs)

    def test_commands_without_expect_is_rejected(self) -> None:
        errs = self.check(self.BASE + 'commands = ["ps"]\n')
        self.assertTrue(any("vacuously" in e for e in errs), errs)

    def test_expect_with_nothing_to_observe_is_rejected(self) -> None:
        errs = self.check('arch = "riscv64"\nboard = "visionfive2"\n[target]\n'
                          'serial = "/dev/ttyUSB0"\nexpect = ["PASS"]\n')
        self.assertTrue(any("nothing would be running" in e for e in errs), errs)

    def test_expect_alone_with_console_check_is_fine(self) -> None:
        self.assertEqual(self.check(self.BASE + 'expect = ["System ready"]\n'), [])

    def test_negative_boot_wait_is_rejected(self) -> None:
        errs = self.check(self.BASE + "boot_wait = -1\n")
        self.assertTrue(any("boot_wait" in e for e in errs), errs)

    def test_boot_timeout_must_carry_the_s_suffix(self) -> None:
        errs = self.check(self.BASE + 'boot_timeout = "90"\n')
        self.assertTrue(any("boot_timeout" in e for e in errs), errs)
        self.assertEqual(self.check(self.BASE + 'boot_timeout = "90s"\n'), [])

    def test_absolute_log_path_is_rejected(self) -> None:
        errs = self.check(self.BASE + 'log = "/tmp/console.log"\n')
        self.assertTrue(any("repository-relative" in e for e in errs), errs)

    def test_nonpositive_baud_is_rejected(self) -> None:
        self.assertTrue(self.check(self.BASE + "baud = 0\n"))

    def test_unknown_target_key_is_rejected(self) -> None:
        """Unknown keys are a structural failure, so they surface as an
        InstanceError from the parser rather than a validate() message."""
        with self.assertRaises(InstanceError) as cm:
            load(self.tmp, self.BASE + "jtag = true\n")
        self.assertIn("jtag", str(cm.exception))

    def test_derive_emits_only_the_fields_make_consumes(self) -> None:
        """Only the two [target] fields a makefile reads become TARGET_* vars.

        The other nine are read by no recipe. The console session, the reset
        pulse, command injection and expect matching are all driven by a20
        straight from the dataclass, so deriving them told `a20 show-vars` that
        make had been handed a configuration it never sees -- and the docs then
        documented that claim as if it were true.
        """
        got = derived(self.tmp, """
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            baud = 115200
            reset = "openocd -c 'init' -c 'reset run'"
            boot_wait = 4
            boot_timeout = "90s"
            console_check = ["System ready", "A20OS"]
            commands = ["ps", "poweroff"]
            expect = ["A20OS"]
            boot_media = ["build/a.img", "build/b.img"]
            log = ".kernel-build/console/x.log"
        """)
        self.assertEqual(got["TARGET_BOOT_MEDIA"], "build/a.img build/b.img")
        for dead in ("TARGET_SERIAL", "TARGET_BAUD", "TARGET_RESET_CMD",
                     "TARGET_BOOT_WAIT", "TARGET_BOOT_TIMEOUT",
                     "TARGET_CONSOLE_CHECK", "TARGET_COMMANDS",
                     "TARGET_EXPECT", "TARGET_CONSOLE_LOG",
                     "TARGET_MEDIA_DEVICE"):
            self.assertNotIn(dead, got, f"{dead} is read by no make recipe")

    def test_a_reset_command_never_reaches_make(self) -> None:
        """The reset command is a20's to run, so it must not cross into make.

        This is the sharp edge of dropping the derivation: make would only ever
        see the string, never run it, but a reset line *looks* like a command a
        recipe might one day execute. Keeping it out of the environment means
        the only thing that can act on it is the code that already shlex-splits
        and runs it without a shell.
        """
        got = derived(self.tmp, """
            arch = "armv7m"
            [target]
            serial = "/dev/ttyUSB0"
            reset = "openocd -c 'reset run'"
        """)
        self.assertNotIn("TARGET_RESET_CMD", got)
        self.assertFalse([k for k in got if "reset" in k.lower()])

    def test_absent_target_emits_no_variables(self) -> None:
        got = derived(self.tmp, 'arch = "riscv64"\n')
        self.assertFalse([k for k in got if k.startswith("TARGET_")])

    def test_partial_target_only_emits_what_is_set(self) -> None:
        """serial alone is a20's own business: no recipe reads it, so no
        TARGET_* variable is derived from it."""
        got = derived(self.tmp, """
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
        """)
        self.assertFalse([k for k in got if k.startswith("TARGET_")])


class FakeTransport:
    """Scripted stand-in for a serial port, so the session logic is testable
    without a board."""

    def __init__(self, script=(), replies=None, read_budget=400):
        self.script = list(script)
        self.replies = replies or {}
        self.sent: list[bytes] = []
        self.closed = False
        self.reads = 0
        self.read_budget = read_budget

    def read(self, _timeout: float) -> bytes:
        # A session that keeps polling without ever reaching its deadline would
        # spin forever under the real clock, turning a regression into a hung
        # suite.  Exhausting the budget makes it a failure instead.
        self.reads += 1
        if self.reads > self.read_budget:
            raise AssertionError("session polled past its deadline without terminating")
        return self.script.pop(0) if self.script else b""

    def write(self, data: bytes) -> None:
        self.sent.append(data)
        for needle, reply in self.replies.items():
            if needle.encode() in data:
                self.script.append(reply)

    def close(self) -> None:
        self.closed = True


class FakeClock:
    """Deterministic clock: the deadline paths then terminate in a couple of
    iterations instead of spinning for a real second, which is both faster and
    the reason a broken deadline shows up as a failure rather than a hang."""

    def __init__(self, step: float = 0.5) -> None:
        self.t = 0.0
        self.step = step

    def now(self) -> float:
        t, self.t = self.t, self.t + self.step
        return t

    def sleep(self, seconds: float) -> None:
        self.t += seconds


class TestConsoleSession(unittest.TestCase):
    """The session decides whether a board actually came up, so it is exercised
    through a fake transport rather than only on hardware."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def inst(self, text: str):
        return load(self.tmp, text)

    def test_boot_signature_then_commands_then_expect(self) -> None:
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            boot_timeout = "2s"
            console_check = ["System ready"]
            commands = ["ps"]
            expect = ["init 0"]
        """)
        t = FakeTransport([b"boot: ", b"System ready\n"], {"ps": b"init 0 root\n"})
        r = run_console_session(t, i, sleep=lambda _s: None)
        self.assertTrue(r.ok, r.missing)
        self.assertEqual(r.stage, "done")
        self.assertEqual(t.sent, [b"ps\n"])
        self.assertTrue(t.closed)

    def test_boot_signature_never_arrives_fails_with_the_missing_pattern(self) -> None:
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            boot_timeout = "1s"
            console_check = ["System ready"]
        """)
        clk = FakeClock()
        r = run_console_session(FakeTransport([b"nothing\n"] * 100), i,
                               sleep=clk.sleep, now=clk.now)
        self.assertFalse(r.ok)
        self.assertEqual(r.stage, "console_check")
        self.assertEqual(r.missing, ("System ready",))

    def test_expect_never_arrives_fails_at_the_expect_stage(self) -> None:
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            boot_timeout = "1s"
            commands = ["ps"]
            expect = ["NEVER"]
        """)
        clk = FakeClock()
        r = run_console_session(FakeTransport([b"garbage\n"] * 200), i,
                               sleep=clk.sleep, now=clk.now)
        self.assertFalse(r.ok)
        self.assertEqual(r.stage, "expect")
        self.assertEqual(r.missing, ("NEVER",))

    def test_transport_is_always_closed_even_on_failure(self) -> None:
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            boot_timeout = "1s"
            console_check = ["NEVER"]
        """)
        t = FakeTransport([b"x\n"] * 100)
        clk = FakeClock()
        run_console_session(t, i, sleep=clk.sleep, now=clk.now)
        self.assertTrue(t.closed)

    def test_every_command_is_sent_with_a_newline(self) -> None:
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            boot_timeout = "1s"
            commands = ["ps", "cat /etc/os-release"]
            expect = ["done"]
        """)
        t = FakeTransport([b"done\n"])
        run_console_session(t, i, sleep=lambda _s: None)
        self.assertEqual(t.sent, [b"ps\ncat /etc/os-release\n"])

    def test_partial_output_is_still_captured_in_the_transcript(self) -> None:
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            boot_timeout = "1s"
            console_check = ["System ready"]
        """)
        r = run_console_session(FakeTransport([b"a\n", b"b\n", b"System ready\n"]), i)
        self.assertTrue(r.ok)
        self.assertIn("a\nb\nSystem ready", r.transcript.text)

    def test_on_output_streams_as_it_arrives(self) -> None:
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            boot_timeout = "1s"
            console_check = ["READY"]
        """)
        seen: list[str] = []
        run_console_session(FakeTransport([b"one ", b"two ", b"READY\n"]), i,
                            on_output=seen.append)
        self.assertEqual("".join(seen), "one two READY\n")

    def test_no_console_check_still_works_as_a_passive_attach(self) -> None:
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
        """)
        t = FakeTransport([b"anything\n"])
        r = run_console_session(t, i, sleep=lambda _s: None)
        self.assertTrue(r.ok)
        self.assertEqual(t.sent, [])

    def test_reset_command_is_run_before_reading(self) -> None:
        from unittest.mock import patch
        import a20_console
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            reset = "myreset --hard"
            boot_timeout = "1s"
            console_check = ["READY"]
        """)
        with patch("a20_console.subprocess.run") as run:
            run.return_value = type("R", (), {"returncode": 0, "stderr": ""})()
            run_console_session(FakeTransport([b"READY\n"]), i, sleep=lambda _s: None)
        self.assertEqual(run.call_args.args[0], ["myreset", "--hard"])

    def test_failing_reset_is_reported_with_its_own_stderr(self) -> None:
        from unittest.mock import patch
        import a20_console
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            reset = "myreset --hard"
        """)
        with patch("a20_console.subprocess.run") as run:
            run.return_value = type("R", (), {"returncode": 3, "stderr": "no adapter"})()
            with self.assertRaises(ConsoleError) as cm:
                run_console_session(FakeTransport(), i, sleep=lambda _s: None)
        self.assertIn("no adapter", str(cm.exception))

    def test_reset_uses_argv_not_a_shell(self) -> None:
        """A manifest must not be able to smuggle in a pipeline via reset."""
        from unittest.mock import patch
        import a20_console
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            reset = "reset-cmd; rm -rf /"
        """)
        with patch("a20_console.subprocess.run") as run:
            run.return_value = type("R", (), {"returncode": 0, "stderr": ""})()
            run_console_session(FakeTransport(), i, sleep=lambda _s: None)
        argv = run.call_args.args[0]
        self.assertEqual(argv, ["reset-cmd;", "rm", "-rf", "/"])

    def test_a_hung_reset_is_bounded(self) -> None:
        """A reset that never returns must not inherit the session's patience.

        `openocd` invoked without a probe will sit there waiting, and a console
        session that inherits that wait hangs on hardware the operator may have
        to power-cycle anyway -- the one case where hanging helps nobody.
        """
        from unittest.mock import patch
        import subprocess
        import a20_console
        i = self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            reset = "openocd -c 'init'"
        """)
        with patch("a20_console.subprocess.run") as run:
            run.side_effect = subprocess.TimeoutExpired(cmd="openocd", timeout=60.0)
            with self.assertRaises(ConsoleError) as cm:
                run_console_session(FakeTransport(), i, sleep=lambda _s: None)
        msg = str(cm.exception)
        self.assertIn("did not finish", msg)
        self.assertIn("openocd", msg)
        self.assertEqual(run.call_args.kwargs["timeout"], a20_console._RESET_TIMEOUT_S)

    def test_duration_parsing(self) -> None:
        from a20_console import _seconds
        self.assertEqual(_seconds("90s", 0), 90.0)
        self.assertEqual(_seconds("1.5s", 0), 1.5)
        self.assertEqual(_seconds(None, 7.0), 7.0)
        for bad in ("90", "s", "abc", "90S"):
            with self.assertRaises(ConsoleError, msg=bad):
                _seconds(bad, 0)

    def test_log_path_defaults_under_the_build_tree(self) -> None:
        from a20_console import console_log_path
        p = console_log_path(self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
        """))
        self.assertEqual(p.name, "case.log")
        self.assertIn(".kernel-build", str(p))
        self.assertTrue(str(p).startswith(str(REPO_ROOT)))

    def test_log_path_honours_an_explicit_relative_override(self) -> None:
        from a20_console import console_log_path
        p = console_log_path(self.inst("""
            arch = "riscv64"
            board = "visionfive2"
            [target]
            serial = "/dev/ttyUSB0"
            log = "mylogs/board.log"
        """))
        self.assertEqual(str(p), str(REPO_ROOT / "mylogs" / "board.log"))


@unittest.skipUnless(hasattr(os, "fork"), "requires pty semantics")
class TestSerialTransportAgainstPty(unittest.TestCase):
    """Exercised against a real tty, since the termios setup is exactly the part
    that cannot be faked.  Each case takes a fresh pty: TIOCEXCL on a pty stays
    latched while its master is open, so a device is never opened twice."""

    def fresh(self):
        master, slave = pty.openpty()
        self.addCleanup(lambda: (os.close(master) if _alive(master) else None))
        self.addCleanup(lambda: (os.close(slave) if _alive(slave) else None))
        return master, os.ttyname(slave)

    def test_opens_and_round_trips_data(self) -> None:
        master, dev = self.fresh()
        t = SerialTransport(dev, 115200)
        self.addCleanup(t.close)
        os.write(master, b"hello from board\n")
        self.assertEqual(t.read(1.0), b"hello from board\n")
        t.write(b"ps\n")
        self.assertEqual(os.read(master, 100), b"ps\n")

    def test_a_board_that_stops_draining_is_named_not_waited_on(self) -> None:
        """A wedged UART must surface as an error, not as a silent hang.

        The retry loop behind write() spins on BlockingIOError while the board
        refuses to accept bytes. Without a deadline that loop is the hang: the
        operator sees nothing at all while holding a board they cannot use.
        """
        from unittest.mock import patch
        _m, dev = self.fresh()
        t = SerialTransport(dev, 115200)
        self.addCleanup(t.close)
        with patch("a20_console.os.write", side_effect=BlockingIOError), \
                patch("a20_console.time.sleep"):
            with self.assertRaises(ConsoleError) as cm:
                t.write(b"ps\n", timeout=0.0)
        msg = str(cm.exception)
        self.assertIn("not draining", msg)
        self.assertIn(dev, msg)

    def test_baud_rate_is_actually_encoded(self) -> None:
        import termios
        for baud in (9600, 38400, 115200, 921600):
            _m, dev = self.fresh()
            t = SerialTransport(dev, baud)
            self.assertEqual(termios.tcgetattr(t._fd)[4], getattr(termios, f"B{baud}"), baud)
            t.close()

    def test_raw_mode_preserves_board_crlf(self) -> None:
        """ICRNL/OPOST would rewrite a board's CRLF, so the expect patterns a
        manifest writes could silently never match."""
        import termios
        master, dev = self.fresh()
        t = SerialTransport(dev, 115200)
        self.addCleanup(t.close)
        attrs = termios.tcgetattr(t._fd)
        self.assertFalse(attrs[3] & termios.ECHO, "ECHO must be off")
        self.assertFalse(attrs[3] & termios.ICANON, "ICANON must be off")
        self.assertFalse(attrs[0] & termios.ICRNL, "ICRNL must be off")
        os.write(master, b"line1\r\nline2\r\n")
        self.assertIn(b"\r\n", t.read(1.0))

    def test_read_returns_promptly_when_nothing_arrives(self) -> None:
        _m, dev = self.fresh()
        t = SerialTransport(dev, 115200)
        self.addCleanup(t.close)
        start = time.monotonic()
        self.assertEqual(t.read(0.4), b"")
        self.assertLess(time.monotonic() - start, 2.0)

    def test_concurrent_opener_is_refused(self) -> None:
        _m, dev = self.fresh()
        first = SerialTransport(dev, 115200)
        self.addCleanup(first.close)
        with self.assertRaises(OSError):
            SerialTransport(dev, 115200)

    def test_unsupported_baud_names_the_alternatives(self) -> None:
        _m, dev = self.fresh()
        with self.assertRaises(ConsoleError) as cm:
            SerialTransport(dev, 12345)
        self.assertIn("115200", str(cm.exception))

    def test_missing_device_raises_oserror(self) -> None:
        with self.assertRaises(OSError):
            SerialTransport("/dev/a20-not-a-real-tty", 115200)


class TestHostPortGate(unittest.TestCase):
    """A host port is a contended resource like memory, so it is declared per
    instance, checked before launch, and waited on -- not defaulted."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def inst(self, hostfwd: str) -> "object":
        return load(self.tmp, f"""
            arch = "riscv64"
            board = "qemu-virt-riscv64"
            [net]
            hostfwd = {hostfwd}
        """)

    def test_parses_fixed_ports(self) -> None:
        from a20_resource import parse_hostfwd_ports
        self.assertEqual(parse_hostfwd_ports(["tcp::5555-:5555"]),
                         (("0.0.0.0", 5555),))

    def test_ephemeral_port_is_not_gated(self) -> None:
        """Port 0 is allocated by the OS at bind time, so nothing is contended
        in advance and there is nothing for a preflight to wait on."""
        from a20_resource import parse_hostfwd_ports
        self.assertEqual(parse_hostfwd_ports(["tcp::0-:5555"]), ())

    def test_bound_address_is_kept(self) -> None:
        from a20_resource import parse_hostfwd_ports
        self.assertEqual(parse_hostfwd_ports(["tcp:127.0.0.1:8080-:80"]),
                         (("127.0.0.1", 8080),))

    def test_malformed_entries_are_ignored_not_fatal(self) -> None:
        from a20_resource import parse_hostfwd_ports
        self.assertEqual(parse_hostfwd_ports(["nonsense", "tcp::x-:1"]), ())

    def test_tcp_and_udp_of_one_instance_yield_one_port(self) -> None:
        from a20_resource import parse_hostfwd_ports
        self.assertEqual(parse_hostfwd_ports(["tcp::5555-:5555", "udp::5555-:5555"]),
                         (("0.0.0.0", 5555), ("0.0.0.0", 5555)))

    def test_busy_ports_detects_a_held_port(self) -> None:
        import socket as sk
        from a20_resource import busy_ports
        held = sk.socket()
        held.bind(("0.0.0.0", 0))
        held.listen(1)
        port = held.getsockname()[1]
        try:
            self.assertIn(("0.0.0.0", port), busy_ports([("0.0.0.0", port)]))
        finally:
            held.close()

    def test_free_port_is_not_reported_busy(self) -> None:
        import socket as sk
        from a20_resource import busy_ports
        probe = sk.socket()
        probe.bind(("0.0.0.0", 0))
        port = probe.getsockname()[1]
        probe.close()
        self.assertEqual(busy_ports([("0.0.0.0", port)]), ())

    def test_requirement_carries_the_declared_ports(self) -> None:
        from a20_resource import Policy, requirement_for
        got = requirement_for(self.inst('["tcp::5555-:5555"]'), Policy())
        self.assertEqual(got.ports, (("0.0.0.0", 5555),))

    def test_instance_without_hostfwd_claims_no_port(self) -> None:
        from a20_resource import Policy, requirement_for
        self.assertEqual(requirement_for(self.inst("[]"), Policy()).ports, ())

    def test_busy_port_is_a_deficit(self) -> None:
        from a20_resource import evaluate
        need = Requirement(mem_mb=1, cpus=1, disk_mb=1, ports=(("0.0.0.0", 5555),))
        have = HostResources(mem_available_mb=99999, cpu_count=64, load1=0.0,
                             disk_free_mb=99999, running_guests=0,
                             busy_ports=(("0.0.0.0", 5555),))
        v = evaluate(need, have, Policy())
        self.assertFalse(v.ok)
        self.assertIn("5555", v.reason())

    def test_preflight_refuses_a_busy_port(self) -> None:
        import socket as sk
        from a20_resource import preflight
        held = sk.socket()
        held.bind(("0.0.0.0", 0))
        held.listen(1)
        port = held.getsockname()[1]
        try:
            inst = self.inst(f'["tcp::{port}-:5555"]')
            with self.assertRaises(A20Error) as cm:
                preflight(inst, Policy(), self.tmp, wait=False)
            self.assertIn(str(port), str(cm.exception))
        finally:
            held.close()

    def test_preflight_waits_then_proceeds_when_the_port_frees(self) -> None:
        import socket as sk
        import threading
        from a20_resource import preflight
        held = sk.socket()
        held.bind(("0.0.0.0", 0))
        held.listen(1)
        port = held.getsockname()[1]
        inst = self.inst(f'["tcp::{port}-:5555"]')

        def release():
            time.sleep(0.2)
            held.close()

        threading.Thread(target=release, daemon=True).start()
        with patch("a20_resource._POLL_SECONDS", 0.05):
            verdict = preflight(inst, Policy(), self.tmp, wait=True, echo=lambda _m: None)
        self.assertTrue(verdict.ok)
