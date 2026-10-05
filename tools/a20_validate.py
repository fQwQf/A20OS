"""Semantic validation for instance manifests.

Mirrors the Makefile's own guards (arch/board/ABI/SMP/NOMMU constraints) plus
the cross-section rules for board-specific sections ([stm32], [flash],
[package]).  Capability arch matrices (which arch may nommu, swap, run the
xlator, ...) are read from components/trim.toml -- the same registry the
generated components/trim.mk feeds the Makefile, so instance validation and
the build cannot disagree.  Value-format policy that the Makefile already
enforces with $(error) — e.g. STM32 bluetooth field formats — stays
Makefile-owned; this module checks structure and compatibility only.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Final, assert_never

from a20_registry import (
    RegistryError,
    TrimRegistry,
    load_flash_backends,
    load_trim_registry,
)
from a20_instance import (
    ABI_CHOICES,
    DRIVER_DEPLOYMENTS,
    KNOWN_ARCHES,
    PACKAGE_KINDS,
    PROFILES,
    QEMU_RUNNABLE_ARCHES,
    RELEASE_ARCH_ARTIFACTS,
    Instance,
    default_board,
    section_is_set,
)

_NAME_RE: Final = re.compile(r"[a-z0-9][a-z0-9_-]*")
_MEMORY_RE: Final = re.compile(r"[0-9]+[KMGT]")
# Accepts the readable short form a manifest writes (tcp::5555-:5555), the fully
# spelled QEMU form (hostfwd=tcp::5555-:5555=on), and an optional host address
# on either side.  The manifests use the short form; NetCfg rewrites it into
# the explicit form before it reaches the QEMU command line, so the validator
# has to be happy with what the author wrote, not with what QEMU ends up given.
_HOSTFWD_RE: Final = re.compile(
    r"(?:hostfwd=)?(?:tcp|udp):[^:,\s]*:\d*-[^:,\s]*:\d+(?:=(?:on|off))?")
_TIMEOUT_RE: Final = re.compile(r"[0-9]+s")

_UEFI_VARIANTS: Final = ("default", "text")
# "extra" was a fourth variant that resolved to `make vf2-extra`, which built
# the source-built extra.img.  That card differed from "sdcard" only by that
# disk, so with the disk gone the variant went too; vf2-sdcard now fills that
# partition from an apk world (VF2_WORLD).  Reject it here rather than letting
# a20 package pass validation and then fail inside make.
_FIT_SDCARD_VARIANTS: Final = ("minimal", "sdcard")
# "vbox" is the VirtualBox ISO, which is never written to a device.
# "rescue-usb" is the same ISO at a stable path a physical PC can boot
# from a USB stick, which is the only deployment route a thin client has.
_GRUB_ISO_VARIANTS: Final = ("vbox", "rescue-usb")


def _trim(inst: Instance, repo_root: Path, e: list[str]) -> TrimRegistry | None:
    """Load the trim registry, folding a load failure into the error list.

    Capability checks need the registry; without it they are skipped rather
    than guessed at, and the load error surfaces alongside the other
    findings instead of aborting the whole validation run.
    """
    try:
        return load_trim_registry(repo_root)
    except RegistryError as err:
        e.append(f"trim registry: {err} (see components/trim.toml)")
        return None


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
    trim = _trim(inst, repo_root, e)
    if trim is not None:
        _validate_capabilities(inst, trim, e)
    if m.smp is not None:
        if m.smp < 1:
            e.append("machine.smp: must be >= 1")
        elif m.smp != 1 and not m.allow_unverified_smp:
            verified_arches = (trim.capabilities["smp-verified-qemu"].arches
                               if trim and "smp-verified-qemu" in trim.capabilities else ())
            verified = inst.arch in verified_arches and inst.board == default_board(inst.arch)
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
    for size_name, size in (("size_mb", r.size_mb),
                            ("ext4_size_mb", r.ext4_size_mb),
                            ("world_size_mb", r.world_size_mb)):
        if size is not None and size < 1:
            e.append(f"rootfs.{size_name}: must be >= 1")
    if n.hostfwd is not None:
        for fwd in n.hostfwd:
            if not _HOSTFWD_RE.fullmatch(fwd):
                e.append(f"net.hostfwd: '{fwd}' must look like tcp::5555-:5555")
    if t.timeout is not None and not _TIMEOUT_RE.fullmatch(t.timeout):
        e.append(f"test.timeout: '{t.timeout}' must match {_TIMEOUT_RE.pattern} (e.g. 45s)")
    has_test = any(x is not None for x in (t.timeout, t.input_delay, t.commands, t.expect))
    if has_test and not t.expect:
        # Without expect the gate can only ever pass vacuously, so require it
        # here rather than letting `a20 test` discover it after the build.
        e.append("test.expect: required when [test] is present (a gate with no "
                 "expect substring can only pass vacuously)")
    for field_name, entries in (("test.commands", t.commands), ("test.expect", t.expect),
                                ("machine.extra_qemu", m.extra_qemu),
                                ("rootfs.drivers", r.drivers)):
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
        e.append("test.*: the a20 test harness drives the serial console and "
                 "cannot combine with gui.enabled")
    _validate_board_sections(inst, e, repo_root)
    _validate_target(inst, e)
    return e


def _validate_capabilities(inst: Instance, trim: TrimRegistry, e: list[str]) -> None:
    """Capability requests vs the components/trim.toml matrices.

    The Makefile force-disables a capability outside its arch list; these
    checks reject the request at check time instead, so an instance never
    carries a knob the build silently drops.  Profile implications are
    checked too: the Makefile forces PROFILE=mcu (with its nommu/bringup
    implications) for a profile arch regardless of what the manifest says,
    so a conflicting kernel.profile would be ignored -- saying so here beats
    a build that quietly does something else.
    """
    k = inst.kernel
    mcu = trim.profiles.get("mcu")
    mcu_arches = mcu.arches if mcu else ()
    if k.profile == "mcu" and inst.arch not in mcu_arches:
        e.append(f"kernel.profile: mcu is only supported for {', '.join(mcu_arches)} "
                 f"([profile.mcu].arches in components/trim.toml), not {inst.arch}")
    if inst.arch in mcu_arches and k.profile is not None and k.profile != "mcu":
        e.append(f"kernel.profile: '{k.profile}' would be ignored -- arch {inst.arch} is "
                 "forced to profile mcu by [profile.mcu].arches in components/trim.toml")
    if k.nommu and inst.arch not in _cap_arches(trim, "nommu"):
        e.append(f"kernel.nommu: unsupported for {inst.arch}; supported: "
                 f"{', '.join(_cap_arches(trim, 'nommu'))}")
    if k.swap and inst.arch not in _cap_arches(trim, "swap"):
        e.append(f"kernel.swap: unsupported for {inst.arch}; supported: "
                 f"{', '.join(_cap_arches(trim, 'swap'))}")
    if k.ramfs_user and inst.arch not in _cap_arches(trim, "ramfs-user"):
        e.append(f"kernel.ramfs_user: supported only for "
                 f"{', '.join(_cap_arches(trim, 'ramfs-user'))}")


def _cap_arches(trim: TrimRegistry, name: str) -> tuple[str, ...]:
    cap = trim.capabilities.get(name)
    return cap.arches if cap else ()


def _validate_target(inst: Instance, e: list[str]) -> None:
    """Rules for [target], the physical board behind the serial cable.

    The theme is that a deploy must be able to fail.  Every field that could
    make the check vacuous -- a board with no console, commands with nothing to
    assert -- is rejected here rather than discovered after a power cycle.
    """
    t = inst.target
    if not section_is_set(t):
        return
    if t.serial is None:
        e.append("target.serial: required when [target] is present; a physical target "
                 "with no console cannot be observed or verified")
    if t.baud is not None and t.baud <= 0:
        e.append("target.baud: must be a positive integer")
    if t.boot_wait is not None and t.boot_wait < 0:
        e.append("target.boot_wait: must not be negative")
    if t.boot_timeout is not None and not _TIMEOUT_RE.fullmatch(t.boot_timeout):
        e.append(f"target.boot_timeout: expected e.g. '90s', got '{t.boot_timeout}'")
    if t.commands is not None and not t.expect:
        e.append("target.expect: required when target.commands is set; without it the "
                 "on-board check can only pass vacuously")
    if t.console_check is not None and not t.console_check:
        e.append("target.console_check: present but empty")
    if t.log is not None and Path(t.log).is_absolute():
        e.append("target.log: must be a repository-relative path, so console logs stay "
                 "portable between machines")
    if t.boot_media is not None and not t.boot_media:
        e.append("target.boot_media: present but empty")
    if t.expect is not None and t.commands is None and t.console_check is None:
        e.append("target.expect: nothing would be running to observe; set target.commands "
                 "or target.console_check as well")


def _validate_flash(inst: Instance, e: list[str], repo_root: Path) -> None:
    """Resolve [flash].tool against the backend registry and check the board.

    A board/backend mismatch is reported here, at `a20 check` time, rather than
    left to the flash action: the build is long and the mistake is destructive,
    so it must be caught before either.
    """
    if not section_is_set(inst.flash):
        return
    tool = inst.flash.tool
    if tool is None:
        e.append("flash.tool: required when [flash] is present")
        return
    try:
        backends = load_flash_backends(repo_root)
    except RegistryError as err:
        e.append(f"flash.tool: cannot resolve backends: {err}")
        return
    backend = next((b for b in backends if b.name == tool), None)
    if backend is None:
        supported = ", ".join(sorted(b.name for b in backends)) or "(none)"
        e.append(f"flash.tool: '{tool}' is not a registered backend; "
                 f"see components/flash-backends.toml (registered: {supported})")
        return
    if inst.board not in backend.boards:
        e.append(f"flash.tool: backend '{tool}' is not validated for board "
                 f"'{inst.board}'; it covers {', '.join(backend.boards)}")
        return
    mismatch = backend.geometry_mismatch(inst.stm32.flash_kb, inst.stm32.ram_kb)
    if mismatch:
        e.append(f"flash.tool: backend '{tool}' is written for a different flash geometry "
                 f"than this manifest declares ({mismatch})")


def _validate_board_sections(inst: Instance, e: list[str], repo_root: Path) -> None:
    """Cross-section rules for [stm32], [flash], and [package]."""
    if section_is_set(inst.stm32) and inst.arch != "armv7m":
        e.append("stm32.*: only valid for arch = \"armv7m\"")
    _validate_flash(inst, e, repo_root)
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
            if (p.variant or "vbox") not in _GRUB_ISO_VARIANTS:
                e.append(f"package.variant: unsupported '{p.variant}' for grub-iso; "
                         f"supported: {', '.join(_GRUB_ISO_VARIANTS)}")
        case "grub-disk":
            if inst.arch != "x86_64":
                e.append("package.kind grub-disk: requires arch = \"x86_64\"")
            if p.variant is not None:
                e.append("package.variant: not used for grub-disk")
            # The whole point is a disk to write, so a manifest that packages one
            # and names no device has nothing to do with it.
            if not inst.target.boot_media or not inst.target.media_device:
                e.append("package.kind grub-disk: needs target.boot_media and "
                         "target.media_device, since the image is meant to be "
                         "written to a disk")
        case "uefi-disk":
            if inst.arch != "x86_64":
                e.append("package.kind uefi-disk: requires arch = \"x86_64\"")
            if p.variant is not None:
                e.append("package.variant: not used for uefi-disk")
            # Same reasoning as grub-disk: the artifact is a disk to write, so a
            # manifest that names no device has nothing to do with it.
            if not inst.target.boot_media or not inst.target.media_device:
                e.append("package.kind uefi-disk: needs target.boot_media and "
                         "target.media_device, since the image is meant to be "
                         "written to a disk")
        case "uefi-image":
            if inst.board != "virtualbox-aarch64":
                e.append("package.kind uefi-image: requires board = \"virtualbox-aarch64\"")
            variant = p.variant or "default"
            if variant not in _UEFI_VARIANTS:
                e.append(f"package.variant: unsupported '{variant}' for uefi-image; "
                         f"supported: {', '.join(_UEFI_VARIANTS)}")
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
        case "kernel-bundle":
            # This kind exists for boards with no writable medium, so boot_media
            # alongside it would leave nothing to write the bundle to.
            if inst.target.boot_media:
                e.append("package.kind kernel-bundle: not used with "
                         "target.boot_media; this kind exists for boards with no medium")
        case unreachable:
            assert_never(unreachable)
    if p.kind != "release" and (p.kernel_out is not None or p.disk_out is not None):
        e.append("package.kernel_out/disk_out: only used with package.kind = \"release\"")
