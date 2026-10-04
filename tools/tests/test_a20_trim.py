"""Tests for the kernel trim registry (components/trim.toml).

The trim registry is the single source of truth for the policy half of kernel
trimming: profiles' curated source lists and capability arch matrices.  These
tests cover the hermetic parts -- parsing, validation, fragment rendering and
staleness -- against a temp-dir registry, plus one intentional read of the
real tree (mirroring TestRepositoryInstances) so a broken checked-in registry
fails its own gate.
"""

from __future__ import annotations

import shutil
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from a20_error import A20Error  # noqa: E402
from a20_instance import parse_instance  # noqa: E402
from a20_make import REPO_ROOT  # noqa: E402
from a20_registry import (  # noqa: E402
    RegistryError,
    check_trim_fragment,
    cross_check_trim_consumers,
    load_trim_registry,
    plan_trim,
    render_trim_fragment,
    validate_trim_registry,
)

MINIMAL_TRIM = """\
[profile.mcu]
arches = ["armv7m"]
opt = "-Os"
cppflags = ["-DCONFIG_MCU"]
nommu = true
sources = ["%(kernel_dir)s/core/panic.c"]

[capability.nommu]
arches = ["armv7m", "riscv64"]

[capability.swap]
arches = ["riscv64"]

[capability.xlator]
arches = ["riscv64"]

[capability.ramfs-user]
arches = ["riscv64"]

[capability.smp-verified-qemu]
arches = ["riscv64"]

[capability.pcie-mmio-alloc]
arches = ["riscv64"]

[capability.pcie-mmio-ecam]
arches = ["riscv64"]
"""


def _write_registry(root: Path, text: str, **subs) -> Path:
    (root / "components").mkdir(parents=True, exist_ok=True)
    path = root / "components" / "trim.toml"
    path.write_text(text % subs if subs else text, encoding="utf-8")
    return path


def _make_root(tmp: Path, text: str = MINIMAL_TRIM) -> Path:
    """A minimal fake tree: registry + one source file + one board."""
    _write_registry(tmp, text, kernel_dir="kernel")
    src = tmp / "kernel" / "core"
    src.mkdir(parents=True, exist_ok=True)
    (src / "panic.c").write_text("/* stub */\n")
    (tmp / "kernel" / "platform" / "licheerv-nano").mkdir(parents=True, exist_ok=True)
    (tmp / "kernel" / "platform" / "licheerv-nano" / "board.c").write_text("/* stub */\n")
    return tmp


class TestTrimRegistry(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_minimal_registry_roundtrip(self):
        root = _make_root(self.tmp)
        reg = load_trim_registry(root)
        self.assertEqual(list(reg.profiles), ["mcu"])
        self.assertEqual(reg.profiles["mcu"].sources, ("kernel/core/panic.c",))
        self.assertTrue(reg.profiles["mcu"].nommu)
        self.assertEqual(reg.capabilities["swap"].arches, ("riscv64",))
        self.assertEqual(validate_trim_registry(reg, root), [])

    def test_missing_required_capability_is_an_error(self):
        root = _make_root(self.tmp, MINIMAL_TRIM.replace('[capability.swap]\narches = ["riscv64"]\n\n', ""))
        reg = load_trim_registry(root)
        errors = validate_trim_registry(reg, root)
        self.assertTrue(any("capability 'swap': required" in e for e in errors))

    def test_unknown_arch_and_missing_source_are_errors(self):
        bad = MINIMAL_TRIM.replace('sources = ["%(kernel_dir)s/core/panic.c"]',
                                   'sources = ["kernel/core/nope.c"]').replace(
                                   '"riscv64"]', '"riscv999"]')
        root = _make_root(self.tmp, bad)
        reg = load_trim_registry(root)
        errors = validate_trim_registry(reg, root)
        self.assertTrue(any("source file missing" in e for e in errors))
        self.assertTrue(any("unknown arch 'riscv999'" in e for e in errors))

    def test_source_outside_kernel_is_rejected(self):
        bad = MINIMAL_TRIM.replace('sources = ["%(kernel_dir)s/core/panic.c"]',
                                   'sources = ["user/rootfs_overlay/init.c"]')
        root = _make_root(self.tmp, bad)
        reg = load_trim_registry(root)
        errors = validate_trim_registry(reg, root)
        self.assertTrue(any("kernel/-relative" in e for e in errors))

    def test_board_exclusion_must_exist(self):
        bad = MINIMAL_TRIM.replace(
            "[capability.pcie-mmio-ecam]\narches = [\"riscv64\"]",
            "[capability.pcie-mmio-ecam]\narches = [\"riscv64\"]\n"
            "board_exclusions = [\"no-such-board\"]")
        root = _make_root(self.tmp, bad)
        reg = load_trim_registry(root)
        errors = validate_trim_registry(reg, root)
        self.assertTrue(any("board 'no-such-board'" in e for e in errors))

    def test_fragment_render_and_staleness(self):
        root = _make_root(self.tmp)
        reg = load_trim_registry(root)
        (root / "components" / "trim.mk").write_text(render_trim_fragment(reg))
        self.assertEqual(check_trim_fragment(reg, root), [])
        path = root / "components" / "trim.mk"
        path.write_text(path.read_text() + "# drifted\n")
        self.assertTrue(check_trim_fragment(reg, root))

    def test_fragment_carries_force_flags_and_cppflags(self):
        root = _make_root(self.tmp)
        text = render_trim_fragment(load_trim_registry(root))
        self.assertIn("TRIM_PROFILE_MCU_ARCHES := armv7m", text)
        self.assertIn("TRIM_PROFILE_MCU_FORCE_NOMMU := 1", text)
        self.assertIn("TRIM_PROFILE_MCU_CPPFLAGS := -DCONFIG_MCU", text)
        self.assertIn("TRIM_CAP_NOMMU_ARCHES := armv7m riscv64", text)

    def test_consumer_cross_check_requires_makefile_reads(self):
        root = _make_root(self.tmp)
        reg = load_trim_registry(root)
        # No makefile in the fake tree mentions the emitted variables.
        errors = cross_check_trim_consumers(reg, root)
        self.assertTrue(any("TRIM_PROFILE_MCU_ARCHES" in e for e in errors))
        # A makefile reading every required variable silences the check.
        (root / "Makefile").write_text(
            "TRIM_PROFILE_MCU_ARCHES TRIM_PROFILE_MCU_OPT TRIM_PROFILE_MCU_CPPFLAGS "
            "TRIM_PROFILE_MCU_SOURCES TRIM_PROFILE_MCU_FORCE_NOMMU "
            "TRIM_CAP_NOMMU_ARCHES TRIM_CAP_SWAP_ARCHES TRIM_CAP_XLATOR_ARCHES "
            "TRIM_CAP_SMP_VERIFIED_QEMU_ARCHES "
            "TRIM_CAP_PCIE_MMIO_ALLOC_ARCHES TRIM_CAP_PCIE_MMIO_ECAM_ARCHES\n")
        self.assertEqual(cross_check_trim_consumers(reg, root), [])

    def test_ramfs_user_needs_no_makefile_consumer(self):
        # ramfs-user's build-side enforcement is the RAMFS_USER_OBJCOPY table,
        # so the consumer cross-check must not demand a TRIM_CAP_RAMFS_USER
        # reader (that is exactly the "second hand list" the registry removed).
        root = _make_root(self.tmp)
        reg = load_trim_registry(root)
        (root / "Makefile").write_text("TRIM_PROFILE_MCU_ARCHES\n")
        errors = cross_check_trim_consumers(reg, root)
        self.assertFalse(any("RAMFS_USER" in e for e in errors))


class TestTrimRepositoryRegistry(unittest.TestCase):
    """The checked-in registry must be valid and its fragment current."""

    def test_real_registry_is_valid_and_current(self):
        reg = load_trim_registry(REPO_ROOT)
        self.assertEqual(validate_trim_registry(reg, REPO_ROOT), [])
        self.assertEqual(check_trim_fragment(reg, REPO_ROOT), [])

    def test_real_registry_matches_the_makefile_gates(self):
        reg = load_trim_registry(REPO_ROOT)
        self.assertEqual(cross_check_trim_consumers(reg, REPO_ROOT), [])


class TestTrimPlan(unittest.TestCase):
    def setUp(self):
        self.reg = load_trim_registry(REPO_ROOT)

    def _plan(self, name: str):
        inst = parse_instance(REPO_ROOT / "instances" / f"{name}.toml")
        return {d.label: d for d in plan_trim(inst, self.reg)}

    def test_stm32_plan_is_mcu(self):
        plan = self._plan("stm32f103")
        self.assertEqual(plan["profile"].state, "mcu")
        self.assertEqual(plan["nommu"].state, "on")
        self.assertEqual(plan["swap"].state, "off")
        self.assertEqual(plan["xlator"].state, "off")
        self.assertEqual(plan["memory"].state, "flash 64 KiB, ram 20 KiB")

    def test_hosted_plan_is_full_with_swap_and_xlator(self):
        plan = self._plan("qemu-riscv64")
        self.assertEqual(plan["profile"].state, "full")
        self.assertEqual(plan["nommu"].state, "off")
        self.assertEqual(plan["swap"].state, "on")
        self.assertEqual(plan["xlator"].state, "on")

    def test_nommu_instance_forces_swap_off(self):
        plan = self._plan("milk-v-duo")
        self.assertEqual(plan["nommu"].state, "on")
        self.assertEqual(plan["swap"].state, "off")


class TestTrimValidation(unittest.TestCase):
    """Instance validation now reads the trim registry."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def _validate(self, text: str) -> list[str]:
        from a20_validate import validate_instance

        path = self.tmp / "case.toml"
        path.write_text(textwrap.dedent(text).lstrip(), encoding="utf-8")
        return validate_instance(parse_instance(path), REPO_ROOT)

    def test_swap_on_unsupported_arch_is_rejected(self):
        errors = self._validate("""
            name = "case"
            arch = "riscv32"
            [kernel]
            swap = true
        """)
        self.assertTrue(any("kernel.swap" in e for e in errors))

    def test_swap_on_supported_arch_passes(self):
        errors = self._validate("""
            name = "case"
            arch = "riscv64"
            [kernel]
            swap = true
        """)
        self.assertEqual(errors, [])

    def test_mcu_profile_off_mcu_arch_is_rejected(self):
        errors = self._validate("""
            name = "case"
            arch = "riscv64"
            [kernel]
            profile = "mcu"
        """)
        self.assertTrue(any("kernel.profile: mcu is only supported for" in e for e in errors))

    def test_conflicting_profile_on_armv7m_is_rejected(self):
        errors = self._validate("""
            name = "case"
            arch = "armv7m"
            [kernel]
            profile = "full"
        """)
        self.assertTrue(any("would be ignored" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
