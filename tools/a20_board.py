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
from a20_error import A20Error
from a20_make import build_instance, exec_make
from a20_registry import FlashBackend, RegistryError, load_flash_backends

FLASH_TARGET_REACHABLE: Final = frozenset({"flash-xuanwu-openocd"})


def run_flash(inst: Instance, backends: tuple[FlashBackend, ...],
              make_args: list[str], dry_run: bool) -> None:
    """Build the firmware, then program the board with the declared backend."""
    name = inst.flash.tool
    if not name:
        raise A20Error(f"{inst.source}: [flash].tool is required for 'a20 flash'")
    backend = next((b for b in backends if b.name == name), None)
    if backend is None:
        supported = ", ".join(sorted(b.name for b in backends)) or "(none)"
        raise A20Error(f"{inst.source}: flash.tool '{name}' is not a registered backend",
                         hint=f"register it in components/flash-backends.toml "
                              f"(known backends: {supported})")
    if inst.board not in backend.boards:
        raise A20Error(
            f"{inst.source}: backend '{name}' is not validated for board "
            f"'{inst.board}'; it covers {', '.join(backend.boards)}")
    mismatch = backend.geometry_mismatch(inst.stm32.flash_kb, inst.stm32.ram_kb)
    if mismatch:
        raise A20Error(
            f"{inst.source}: backend '{name}' is written for a different flash "
            f"geometry than this manifest declares ({mismatch})",
            hint="erasing with the wrong geometry runs off the end of the part, "
                 "so this is refused")
    build_instance(inst, list(make_args), dry_run)
    exec_make(inst, backend.make_target, list(make_args), dry_run)


def run_package(inst: Instance, make_args: list[str], dry_run: bool) -> None:
    """Build and assemble the artifact declared by [package].kind."""
    kind = inst.package.kind
    if kind is None:
        raise A20Error(f"{inst.source}: [package] kind is required for 'a20 package'")
    match kind:
        case "grub-iso":
            # A physical PC boots the same ISO from a USB stick, so it needs the
            # artifact at a stable path an instance can name in boot_media; the
            # VirtualBox one lives in the per-build directory and is never
            # written to a device.
            if (inst.package.variant or "vbox") == "rescue-usb":
                exec_make(inst, "pc-rescue-iso", list(make_args), dry_run)
            else:
                exec_make(inst, "_vbox_iso_x86_64_impl", list(make_args), dry_run)
        case "grub-disk":
            # A directly bootable raw disk rather than an ISO: the same shape the
            # aarch64 VirtualBox already produces, so both architectures hand an
            # operator one disk to attach instead of optical media.  It boots via
            # GRUB, which starts from BIOS or UEFI alike.
            exec_make(inst, "pc-rescue-disk", list(make_args), dry_run)
        case "uefi-disk":
            # ESP holds BOOTX64.EFI instead of GRUB.  GRUB 2.12 does not fill
            # the multiboot ACPI tags, so a multiboot kernel booted by GRUB under
            # UEFI never gets the RSDP and cannot find MCFG; our loader reads it
            # from the firmware configuration table instead.
            #
            # The make target stages the image under build/x86_64-pc/ rather than
            # leaving it in the per-build directory, because an instance that
            # deploys this disk has to name one stable path in boot_media.
            exec_make(inst, "x86_64-uefi-disk", list(make_args), dry_run)
        case "uefi-image":
            variant = inst.package.variant or "default"
            target = {"default": "_vbox_image_aarch64_impl",
                      "text": "_vbox_text_image_aarch64_impl"}[variant]
            exec_make(inst, target, list(make_args), dry_run)
        case "kernel-bundle":
            # A board with no block driver: the artifact is the kernel plus the
            # boot-chain commands to load it.  mk_kernel_bundle.sh takes the
            # load address from the ELF, so nothing about the board is restated
            # here or in the manifest.
            exec_make(inst, "kernel-bundle", list(make_args), dry_run)
        case "fit-sdcard":
            # VF2 image assembly (firmware check, extra partition variants)
            # is orchestrated by the vf2-* make targets; the instance carries
            # the validated board/arch identity.  a20_validate rejects any
            # variant that is not one of these targets.
            exec_make(inst, f"vf2-{inst.package.variant}", list(make_args), dry_run)
        case "release":
            default_kernel, default_disk = RELEASE_ARCH_ARTIFACTS[inst.arch]
            kernel_out = inst.package.kernel_out or default_kernel
            disk_out = inst.package.disk_out or default_disk
            extra = [*make_args, f"KERNEL_OUT={kernel_out}", f"DISK_OUT={disk_out}"]
            exec_make(inst, "_release_build", extra, dry_run)
        case unreachable:
            assert_never(unreachable)
