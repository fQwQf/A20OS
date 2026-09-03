"""Semantic validation for instance manifests.

Mirrors the Makefile's own guards (arch/board/ABI/SMP/NOMMU constraints) plus
the cross-section rules for board-specific sections ([stm32], [flash],
[package]).  Value-format policy that the Makefile already enforces with
$(error) — e.g. STM32 bluetooth field formats — stays Makefile-owned; this
module checks structure and compatibility only.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Final, assert_never

from a20_instance import (
    ABI_CHOICES,
    DRIVER_DEPLOYMENTS,
    FLASH_TOOLS,
    KNOWN_ARCHES,
    NOMMU_ARCHES,
    PACKAGE_KINDS,
    PROFILES,
    QEMU_RUNNABLE_ARCHES,
    RAMFS_USER_ARCHES,
    RELEASE_ARCH_ARTIFACTS,
    SMP_VERIFIED_QEMU_ARCHES,
    Instance,
    default_board,
    section_is_set,
)

_NAME_RE: Final = re.compile(r"[a-z0-9][a-z0-9_-]*")
_MEMORY_RE: Final = re.compile(r"[0-9]+[KMGT]")
_HOSTFWD_RE: Final = re.compile(r"(tcp|udp)::[0-9]*-[0-9]*:[0-9]+")
_TIMEOUT_RE: Final = re.compile(r"[0-9]+s")

_UEFI_VARIANTS: Final = ("default", "text", "gui")
_FIT_SDCARD_VARIANTS: Final = ("minimal", "sdcard", "extra")


def validate_instance(inst: Instance, repo_root: Path) -> list[str]:
    """Semantic cross-checks mirroring the Makefile's own guards."""
    e: list[str] = []
    k, m, g, n, r, t = inst.kernel, inst.machine, inst.gui, inst.net, inst.rootfs, inst.test
    if inst.arch not in KNOWN_ARCHES:
        e.append(f"arch: unsupported '{inst.arch}'; supported: {', '.join(KNOWN_ARCHES)}")
    if not _NAME_RE.fullmatch(inst.name):
        e.append(f"name: '{inst.name}' must match {_NAME_RE.pattern}")
    if inst.board and not (repo_root / "kernel" / "platform" / inst.board).is_dir():
        e.append(f"board: no kernel/platform/{inst.board} directory")
    if inst.abi is not None and inst.abi not in ABI_CHOICES:
        e.append(f"abi: unsupported '{inst.abi}'; supported: {', '.join(ABI_CHOICES)}")
    if k.profile is not None and k.profile not in PROFILES:
        e.append(f"kernel.profile: unsupported '{k.profile}'; supported: {', '.join(PROFILES)}")
    if k.driver_deployment is not None and k.driver_deployment not in DRIVER_DEPLOYMENTS:
        e.append(f"kernel.driver_deployment: unsupported '{k.driver_deployment}'; "
                 f"supported: {', '.join(DRIVER_DEPLOYMENTS)}")
    if k.nommu and inst.arch not in NOMMU_ARCHES:
        e.append(f"kernel.nommu: unsupported for {inst.arch}; supported: {', '.join(NOMMU_ARCHES)}")
    if k.ramfs_user and inst.arch not in RAMFS_USER_ARCHES:
        e.append(f"kernel.ramfs_user: supported only for {', '.join(RAMFS_USER_ARCHES)}")
    if m.smp is not None:
        if m.smp < 1:
            e.append("machine.smp: must be >= 1")
        elif m.smp != 1 and not m.allow_unverified_smp:
            verified = inst.arch in SMP_VERIFIED_QEMU_ARCHES and inst.board == default_board(inst.arch)
            if not verified:
                e.append(f"machine.smp={m.smp}: unverified for {inst.arch}/{inst.board}; "
                         "set machine.allow_unverified_smp = true only for explicit SMP bringup")
    if m.memory is not None and not _MEMORY_RE.fullmatch(m.memory):
        e.append(f"machine.memory: '{m.memory}' must match {_MEMORY_RE.pattern} (e.g. 1G)")
    if g.enabled:
        if k.bringup:
            e.append("gui.enabled: cannot combine with kernel.bringup (no rootfs in bringup mode)")
        if inst.arch not in QEMU_RUNNABLE_ARCHES:
            e.append(f"gui.enabled: no QEMU GUI path for {inst.arch}")
    for size_name, size in (("size_mb", r.size_mb), ("gui_size_mb", r.gui_size_mb),
                            ("ext4_size_mb", r.ext4_size_mb), ("extra_size_mb", r.extra_size_mb)):
        if size is not None and size < 1:
            e.append(f"rootfs.{size_name}: must be >= 1")
    if n.hostfwd is not None:
        for fwd in n.hostfwd:
            if not _HOSTFWD_RE.fullmatch(fwd):
                e.append(f"net.hostfwd: '{fwd}' must look like tcp::5555-:5555")
    if t.timeout is not None and not _TIMEOUT_RE.fullmatch(t.timeout):
        e.append(f"test.timeout: '{t.timeout}' must match {_TIMEOUT_RE.pattern} (e.g. 45s)")
    has_test = any(x is not None for x in (t.timeout, t.input_delay, t.commands, t.expect))
    for field_name, entries in (("test.commands", t.commands), ("test.expect", t.expect),
                                ("machine.extra_qemu", m.extra_qemu),
                                ("rootfs.drivers", r.drivers),
                                ("rootfs.extra_packages", r.extra_packages)):
        if entries is not None and any(not s for s in entries):
            e.append(f"{field_name}: entries must be non-empty strings")
    if r.world is not None and not (repo_root / "packages" / "world" / f"{r.world}.world").is_file():
        e.append(f"rootfs.world: no packages/world/{r.world}.world")
    if r.world is not None:
        if k.bringup:
            e.append("rootfs.world: cannot combine with kernel.bringup (world images carry userspace)")
        if inst.arch in ("armv7m", "loongarch32"):
            e.append(f"rootfs.world: apk world images are only supported on hosted arches, not {inst.arch}")
        if has_test:
            e.append("rootfs.world: cannot combine with [test] (world images boot via run-world, "
                     "which the a20 test harness does not drive)")
    if r.world is None:
        if r.world_size_mb is not None:
            e.append("rootfs.world_size_mb: only meaningful together with rootfs.world")
        if r.alpine is not None:
            e.append("rootfs.alpine: only meaningful together with rootfs.world")
    if has_test and g.enabled:
        e.append("test.*: the a20 test harness drives the serial console; "
                 "GUI smokes use tools/smoke_qemu_gui.py and cannot combine with [test]")
    _validate_board_sections(inst, e)
    return e


def _validate_board_sections(inst: Instance, e: list[str]) -> None:
    """Cross-section rules for [stm32], [flash], and [package]."""
    if section_is_set(inst.stm32) and inst.arch != "armv7m":
        e.append("stm32.*: only valid for arch = \"armv7m\"")
    if section_is_set(inst.flash):
        if inst.arch != "armv7m":
            e.append("flash.*: only the armv7m/STM32 OpenOCD flow is currently supported")
        if inst.flash.tool is not None and inst.flash.tool not in FLASH_TOOLS:
            e.append(f"flash.tool: unsupported '{inst.flash.tool}'; supported: {', '.join(FLASH_TOOLS)}")
    p = inst.package
    if not section_is_set(p):
        return
    if p.kind is None:
        e.append("package.kind: required when [package] is present")
        return
    if p.kind not in PACKAGE_KINDS:
        e.append(f"package.kind: unsupported '{p.kind}'; supported: {', '.join(PACKAGE_KINDS)}")
        return
    match p.kind:
        case "grub-iso":
            if inst.arch != "x86_64":
                e.append("package.kind grub-iso: requires arch = \"x86_64\"")
        case "uefi-image":
            if inst.board != "virtualbox-aarch64":
                e.append("package.kind uefi-image: requires board = \"virtualbox-aarch64\"")
            variant = p.variant or "default"
            if variant not in _UEFI_VARIANTS:
                e.append(f"package.variant: unsupported '{variant}' for uefi-image; "
                         f"supported: {', '.join(_UEFI_VARIANTS)}")
            if variant == "gui" and not inst.gui.enabled:
                e.append("package.variant \"gui\": requires gui.enabled = true")
        case "fit-sdcard":
            if inst.board != "visionfive2":
                e.append("package.kind fit-sdcard: requires board = \"visionfive2\"")
            if p.variant not in _FIT_SDCARD_VARIANTS:
                e.append(f"package.variant: required for fit-sdcard; "
                         f"supported: {', '.join(_FIT_SDCARD_VARIANTS)}")
        case "release":
            if inst.arch not in RELEASE_ARCH_ARTIFACTS:
                e.append(f"package.kind release: supported arches: {', '.join(RELEASE_ARCH_ARTIFACTS)}")
            if p.variant is not None:
                e.append("package.variant: not used for release")
        case unreachable:
            assert_never(unreachable)
    if p.kind != "release" and (p.kernel_out is not None or p.disk_out is not None):
        e.append("package.kernel_out/disk_out: only used with package.kind = \"release\"")
