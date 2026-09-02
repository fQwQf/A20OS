"""Instance manifest model and parser.

An instance manifest is a TOML file under instances/ declaring ONE
buildable/runnable A20OS configuration. This module is the trust boundary:
raw TOML dicts enter through parse_instance() and leave as a frozen
Instance; everything downstream works only with the typed model.
Semantic validation lives in a20_validate.py, make-variable derivation in
a20_derive.py.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, fields
from pathlib import Path
from typing import Final, assert_never

KNOWN_ARCHES: Final = (
    "riscv64", "loongarch64", "aarch64", "x86_64",
    "arm32", "riscv32", "ppc64le", "armv7m", "loongarch32",
)
# Arches bootable through the generic QEMU _run_impl flow. armv7m runs
# through the stm32vldiscovery path ([stm32] qemu = true) and loongarch32
# runs on the cemu simulator, so neither is in this list.
QEMU_RUNNABLE_ARCHES: Final = (
    "riscv64", "loongarch64", "aarch64", "x86_64", "arm32", "riscv32", "ppc64le",
)
NOMMU_ARCHES: Final = ("riscv64", "riscv32", "aarch64", "arm32", "armv7m")
SMP_VERIFIED_QEMU_ARCHES: Final = ("riscv64", "aarch64", "loongarch64", "x86_64")
RAMFS_USER_ARCHES: Final = ("loongarch64",)
ABI_CHOICES: Final = ("linux", "native", "both")
DRIVER_DEPLOYMENTS: Final = ("generic", "embedded")
PROFILES: Final = ("full", "benchmark", "mcu")
PACKAGE_KINDS: Final = ("grub-iso", "uefi-image", "fit-sdcard", "release")
FLASH_TOOLS: Final = ("openocd",)
RELEASE_ARCH_ARTIFACTS: Final = {
    "riscv64": ("kernel-rv", "disk.img"),
    "loongarch64": ("kernel-la", "disk-la.img"),
}


@dataclass(frozen=True, slots=True)
class InstanceError(Exception):
    """Structural parse failure of one manifest; carries every error found."""

    errors: tuple[str, ...]

    def __str__(self) -> str:
        return "\n".join(f"  - {e}" for e in self.errors)


@dataclass(frozen=True, slots=True)
class KernelCfg:
    profile: str | None = None
    opt: str | None = None
    user_opt: str | None = None
    bringup: bool | None = None
    nommu: bool | None = None
    driver_deployment: str | None = None
    ubsan: bool | None = None
    swap: bool | None = None
    werror: bool | None = None
    cooperative_boot: bool | None = None
    storage_read_only: bool | None = None
    external_root: bool | None = None
    ramfs_user: bool | None = None


@dataclass(frozen=True, slots=True)
class MachineCfg:
    smp: int | None = None
    memory: str | None = None
    allow_unverified_smp: bool | None = None
    extra_qemu: tuple[str, ...] | None = None


@dataclass(frozen=True, slots=True)
class GuiCfg:
    enabled: bool | None = None
    display: str | None = None
    audio_driver: str | None = None
    audio_device: str | None = None
    frame_window: int | None = None


@dataclass(frozen=True, slots=True)
class NetCfg:
    hostfwd: tuple[str, ...] | None = None


@dataclass(frozen=True, slots=True)
class RootfsCfg:
    size_mb: int | None = None
    gui_size_mb: int | None = None
    ext4_size_mb: int | None = None
    extra_size_mb: int | None = None
    world: str | None = None
    extra_packages: tuple[str, ...] | None = None
    drivers: tuple[str, ...] | None = None


@dataclass(frozen=True, slots=True)
class TestCfg:
    timeout: str | None = None
    input_delay: int | None = None
    commands: tuple[str, ...] | None = None
    expect: tuple[str, ...] | None = None


@dataclass(frozen=True, slots=True)
class Stm32Cfg:
    """STM32F103 (armv7m) board variant knobs; map to STM32_* make variables."""

    flash_kb: int | None = None
    ram_kb: int | None = None
    xuanwu: bool | None = None
    qemu: bool | None = None
    bt_name: str | None = None
    bt_pin: str | None = None
    bt_uuid: str | None = None
    bt_baud: int | None = None
    wifi_ssid: str | None = None
    wifi_password: str | None = None


@dataclass(frozen=True, slots=True)
class FlashCfg:
    """Hardware flashing configuration for `a20 flash`."""

    tool: str | None = None
    interface: str | None = None
    transport: str | None = None
    adapter_khz: int | None = None
    serial: str | None = None


@dataclass(frozen=True, slots=True)
class PackageCfg:
    """Artifact packaging configuration for `a20 package`."""

    kind: str | None = None
    variant: str | None = None
    kernel_out: str | None = None
    disk_out: str | None = None


@dataclass(frozen=True, slots=True)
class Instance:
    name: str
    arch: str
    board: str
    description: str | None
    abi: str | None
    kernel: KernelCfg
    machine: MachineCfg
    gui: GuiCfg
    net: NetCfg
    rootfs: RootfsCfg
    test: TestCfg
    stm32: Stm32Cfg
    flash: FlashCfg
    package: PackageCfg
    source: Path


CfgSection = KernelCfg | MachineCfg | GuiCfg | NetCfg | RootfsCfg | TestCfg | Stm32Cfg | FlashCfg | PackageCfg


def section_is_set(cfg: CfgSection) -> bool:
    """True when any field of a config section dataclass was set in the manifest."""
    return any(getattr(cfg, f.name) is not None for f in fields(cfg))


def default_board(arch: str) -> str:
    if arch == "armv7m":
        return "stm32f103"
    return f"qemu-virt-{arch}"


_TOP_SPECS: Final = {"name": "str", "description": "str", "arch": "str", "board": "str", "abi": "str"}
_SECTION_SPECS: Final = {
    "kernel": {f: ("bool" if f in {
        "bringup", "nommu", "ubsan", "swap", "werror",
        "cooperative_boot", "storage_read_only", "external_root", "ramfs_user",
    } else "str") for f in KernelCfg.__dataclass_fields__},
    "machine": {f: ("int" if f == "smp" else "bool" if f == "allow_unverified_smp"
                    else "str_list" if f == "extra_qemu" else "str")
                for f in MachineCfg.__dataclass_fields__},
    "gui": {f: ("bool" if f == "enabled" else "int" if f == "frame_window" else "str")
            for f in GuiCfg.__dataclass_fields__},
    "net": {f: "str_list" for f in NetCfg.__dataclass_fields__},
    "rootfs": {f: ("str_list" if f in ("extra_packages", "drivers")
                   else "str" if f == "world" else "int")
               for f in RootfsCfg.__dataclass_fields__},
    "test": {f: ("int" if f == "input_delay" else "str_list" if f in ("commands", "expect") else "str")
             for f in TestCfg.__dataclass_fields__},
    "stm32": {f: ("int" if f in ("flash_kb", "ram_kb", "bt_baud")
                  else "bool" if f in ("xuanwu", "qemu") else "str")
              for f in Stm32Cfg.__dataclass_fields__},
    "flash": {f: ("int" if f == "adapter_khz" else "str") for f in FlashCfg.__dataclass_fields__},
    "package": {f: "str" for f in PackageCfg.__dataclass_fields__},
}


def _kind_ok(kind: str, value: object) -> bool:  # noqa: OBJECT_OK -- TOML boundary value, narrowed below
    match kind:
        case "str":
            return isinstance(value, str)
        case "int":
            return isinstance(value, int) and not isinstance(value, bool)
        case "bool":
            return isinstance(value, bool)
        case "str_list":
            return isinstance(value, list) and all(isinstance(v, str) for v in value)
        case unreachable:
            assert_never(unreachable)


def _parse_table(raw: dict[str, object], specs: dict[str, str], where: str,  # noqa: OBJECT_OK -- TOML boundary
                 errors: list[str]) -> dict[str, object]:  # noqa: OBJECT_OK, DICT_OK -- internal scratch
    out: dict[str, object] = {}  # noqa: OBJECT_OK -- TOML boundary scratch
    for key, value in raw.items():
        kind = specs.get(key)
        if kind is None:
            errors.append(f"{where}: unknown key '{key}'")
        elif _kind_ok(kind, value):
            out[key] = tuple(value) if kind == "str_list" else value
        else:
            errors.append(f"{where}.{key}: expected {kind}, got {type(value).__name__}")
    return out


def parse_instance(path: Path) -> Instance:
    """Parse a TOML manifest into a typed Instance or raise InstanceError."""
    try:
        with path.open("rb") as f:
            raw: dict[str, object] = tomllib.load(f)  # noqa: OBJECT_OK -- TOML boundary, narrowed by _parse_table
    except tomllib.TOMLDecodeError as e:
        raise InstanceError(errors=(f"{path.name}: TOML syntax error: {e}",)) from None
    except OSError as e:
        raise InstanceError(errors=(f"{path.name}: {e}",)) from None

    errors: list[str] = []
    raw = dict(raw)
    sections: dict[str, dict[str, object]] = {}  # noqa: OBJECT_OK -- TOML boundary scratch
    for key in list(raw):
        if key in _SECTION_SPECS:
            table = raw.pop(key)
            if isinstance(table, dict):
                sections[key] = _parse_table(table, _SECTION_SPECS[key], key, errors)
            else:
                errors.append(f"[{key}]: expected a TOML table")
    top = _parse_table(raw, _TOP_SPECS, "top-level", errors)

    arch = top.get("arch")
    if not isinstance(arch, str):
        errors.append("top-level.arch: required (string)")
        arch = ""
    board = top.get("board")
    if not isinstance(board, str):
        board = default_board(arch) if arch in KNOWN_ARCHES else ""
    name = top.get("name")
    if not isinstance(name, str):
        name = path.stem
    description = top.get("description")
    abi = top.get("abi")

    if errors:
        raise InstanceError(errors=tuple(errors))
    return Instance(
        name=name,
        arch=arch,
        board=board,
        description=description if isinstance(description, str) else None,
        abi=abi if isinstance(abi, str) else None,
        kernel=KernelCfg(**sections.get("kernel", {})),
        machine=MachineCfg(**sections.get("machine", {})),
        gui=GuiCfg(**sections.get("gui", {})),
        net=NetCfg(**sections.get("net", {})),
        rootfs=RootfsCfg(**sections.get("rootfs", {})),
        test=TestCfg(**sections.get("test", {})),
        stm32=Stm32Cfg(**sections.get("stm32", {})),
        flash=FlashCfg(**sections.get("flash", {})),
        package=PackageCfg(**sections.get("package", {})),
        source=path,
    )
