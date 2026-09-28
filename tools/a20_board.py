"""Board and artifact actions for a20: hardware flashing and image packaging.

These actions cover the flows that are not QEMU runs: programming a board's
flash, GRUB/UEFI VirtualBox images, VisionFive 2 FIT SD cards, and release
artifact assembly.  Configuration comes from the instance; which programmer to
use comes from components/flash-backends.toml; the recipe itself stays in the
Makefile target the backend names.
"""

from __future__ import annotations

from typing import Final, assert_never

from a20_instance import RELEASE_ARCH_ARTIFACTS, Instance
from a20_make import build_instance, exec_make
from a20_registry import FlashBackend, RegistryError, load_flash_backends

FLASH_TARGET_REACHABLE: Final = frozenset({"flash-xuanwu-openocd"})


def run_flash(inst: Instance, backends: tuple[FlashBackend, ...],
              make_args: list[str], dry_run: bool) -> int:
    """Build the firmware, then program the board with the declared backend."""
    name = inst.flash.tool
    if not name:
        raise SystemExit(f"error: {inst.source}: [flash].tool is required for 'a20 flash'")
    backend = next((b for b in backends if b.name == name), None)
    if backend is None:
        supported = ", ".join(sorted(b.name for b in backends)) or "(none)"
        raise SystemExit(f"error: {inst.source}: flash.tool '{name}' is not a registered "
                         f"backend; see components/flash-backends.toml (registered: {supported})")
    if inst.board not in backend.boards:
        raise SystemExit(
            f"error: {inst.source}: backend '{name}' is not validated for board "
            f"'{inst.board}'; it covers {', '.join(backend.boards)}")
    mismatch = backend.geometry_mismatch(inst.stm32.flash_kb, inst.stm32.ram_kb)
    if mismatch:
        raise SystemExit(
            f"error: {inst.source}: backend '{name}' is written for a different flash "
            f"geometry than this manifest declares ({mismatch}). Erasing with the wrong "
            f"geometry runs off the end of the part, so this is refused.")
    build_rc = build_instance(inst, list(make_args), dry_run)
    if build_rc != 0:
        return build_rc
    return exec_make(inst, backend.make_target, [], dry_run)


def run_package(inst: Instance, make_args: list[str], dry_run: bool) -> int:
    """Build and assemble the artifact declared by [package].kind."""
    kind = inst.package.kind
    if kind is None:
        raise SystemExit(f"error: {inst.source}: [package] kind is required for 'a20 package'")
    match kind:
        case "grub-iso":
            return exec_make(inst, "_vbox_iso_x86_64_impl", list(make_args), dry_run)
        case "uefi-image":
            variant = inst.package.variant or "default"
            target = {"default": "_vbox_image_aarch64_impl",
                      "text": "_vbox_text_image_aarch64_impl"}[variant]
            return exec_make(inst, target, list(make_args), dry_run)
        case "fit-sdcard":
            # VF2 image assembly (firmware check, extra partition variants)
            # is orchestrated by the vf2-* make targets; the instance carries
            # the validated board/arch identity.
            return exec_make(inst, f"vf2-{inst.package.variant}", list(make_args), dry_run)
        case "release":
            default_kernel, default_disk = RELEASE_ARCH_ARTIFACTS[inst.arch]
            kernel_out = inst.package.kernel_out or default_kernel
            disk_out = inst.package.disk_out or default_disk
            extra = [*make_args, f"KERNEL_OUT={kernel_out}", f"DISK_OUT={disk_out}"]
            return exec_make(inst, "_release_build", extra, dry_run)
        case unreachable:
            assert_never(unreachable)
