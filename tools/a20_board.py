"""Board and artifact actions for a20: hardware flashing and image packaging.

These actions cover the flows that are not QEMU runs: OpenOCD flashing for
STM32, GRUB/UEFI VirtualBox images, VisionFive 2 FIT SD cards, and release
artifact assembly.  Configuration comes from the instance; orchestration
stays in the Makefile targets they invoke.
"""

from __future__ import annotations

from typing import assert_never

from a20_instance import RELEASE_ARCH_ARTIFACTS, Instance
from a20_make import build_instance, exec_make


def run_flash(inst: Instance, make_args: list[str], dry_run: bool) -> int:
    """Build the firmware and flash it to the board (OpenOCD)."""
    if not inst.flash.tool:
        raise SystemExit(f"error: {inst.source}: [flash] section with tool is required for 'a20 flash'")
    build_rc = build_instance(inst, list(make_args), dry_run)
    if build_rc != 0:
        return build_rc
    return exec_make(inst, "flash-xuanwu-openocd", [], dry_run)


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
                      "text": "_vbox_text_image_aarch64_impl",
                      "gui": "_vbox_gui_image_aarch64_impl"}[variant]
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
