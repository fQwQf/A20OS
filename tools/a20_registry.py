"""Component registries (components/*.toml).

Two registries live here and share one TOML table parser, because they answer
the same shape of question -- "what exists, and does the build agree?" -- over
different vocabularies.

`components/drivers.toml` declares which .a20drv driver packages exist, which
architectures each supports, and which are embedded early into the kernel root
ramfs.  `a20 check-registry` cross-checks it against the Makefile's own build
lists (DRVMOD_MODULES / EARLY_DRVMOD_MODULES).

`components/flash-backends.toml` declares which ways of programming a board's
non-volatile memory exist, which boards each is validated for, and which make
target implements it.  `boards` is a safety allowlist, not documentation: a
manifest naming a board outside it is rejected before anything is built.
"""

from __future__ import annotations

import re
import subprocess
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Final

from a20_instance import KNOWN_ARCHES, Instance

# Architectures where DRIVER_DEPLOYMENT=generic (loadable modules) is supported
# and tools/driver-modules.mk defines per-arch module lists.
GENERIC_DEPLOYMENT_ARCHES: Final = ("riscv64", "x86_64", "aarch64", "loongarch64")

_DRIVER_KEYS: Final = {"name": "str", "source": "str", "description": "str",
                       "arches": "str_list", "early_arches": "str_list"}

_FLASH_BACKEND_KEYS: Final = {"name": "str", "description": "str",
                              "boards": "str_list", "make_target": "str",
                              "flash_kb": "int", "ram_kb": "int"}


@dataclass(frozen=True, slots=True)
class DriverComponent:
    name: str
    source: str
    arches: tuple[str, ...]
    early_arches: tuple[str, ...]
    description: str | None

    @property
    def package(self) -> str:
        return f"{self.name}.a20drv"


@dataclass(frozen=True, slots=True)
class FlashBackend:
    name: str
    boards: tuple[str, ...]
    make_target: str
    description: str | None
    flash_kb: int | None = None
    ram_kb: int | None = None

    def geometry_mismatch(self, flash_kb: int | None, ram_kb: int | None) -> str | None:
        """Describe how an instance's flash geometry departs from this recipe's, if it does."""
        bad = [f"{label} {want} != manifest {got}"
               for label, want, got in (("flash_kb", self.flash_kb, flash_kb),
                                        ("ram_kb", self.ram_kb, ram_kb))
               if want is not None and got is not None and want != got]
        return ", ".join(bad) if bad else None


@dataclass(frozen=True, slots=True)
class RegistryError(Exception):
    """Structural parse failure of a registry; carries every error found."""

    errors: tuple[str, ...]

    def __str__(self) -> str:
        return "\n".join(f"  - {e}" for e in self.errors)


def _parse_array_of_tables(path: Path, array_key: str, spec: dict[str, str]) -> list[dict[str, Any]]:
    """Read one TOML array-of-tables into a list of typed field dicts.

    Accumulates every structural problem instead of failing on the first, so a
    malformed registry reports all of its errors in one run.
    """
    try:
        with path.open("rb") as f:
            raw = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        raise RegistryError(errors=(f"{path.name}: TOML syntax error: {e}",)) from None
    except OSError as e:
        raise RegistryError(errors=(f"{path.name}: {e}",)) from None

    errors: list[str] = []
    table = raw.get(array_key)
    if not isinstance(table, list):
        raise RegistryError(errors=(f"{path.name}: expected a [[{array_key}]] array of tables",))
    out: list[dict[str, Any]] = []
    for i, item in enumerate(table):
        where = f"{array_key}[{i}]"
        if not isinstance(item, dict):
            errors.append(f"{where}: expected a table")
            continue
        fields: dict[str, Any] = {}
        for key, value in item.items():
            kind = spec.get(key)
            if kind is None:
                errors.append(f"{where}: unknown key '{key}'")
                continue
            if kind == "str":
                ok = isinstance(value, str)
            elif kind == "int":
                ok = isinstance(value, int) and not isinstance(value, bool)
            else:
                ok = isinstance(value, list) and all(isinstance(v, str) for v in value)
            if ok:
                fields[key] = tuple(value) if kind == "str_list" else value
            else:
                errors.append(f"{where}.{key}: expected {kind}, got {type(value).__name__}")
        out.append(fields)
    if errors:
        raise RegistryError(errors=tuple(errors))
    return out


def _str_list(fields: dict[str, Any], key: str) -> tuple[str, ...]:
    value = fields.get(key)
    return value if isinstance(value, tuple) else ()


def _str(fields: dict[str, Any], key: str) -> str | None:
    value = fields.get(key)
    return value if isinstance(value, str) else None


def registry_path(repo_root: Path) -> Path:
    return repo_root / "components" / "drivers.toml"


def flash_backends_path(repo_root: Path) -> Path:
    return repo_root / "components" / "flash-backends.toml"


def load_registry(repo_root: Path) -> tuple[DriverComponent, ...]:
    """Parse the driver registry TOML into typed components or raise RegistryError."""
    path = registry_path(repo_root)
    errors: list[str] = []
    entries: list[DriverComponent] = []
    for i, fields in enumerate(_parse_array_of_tables(path, "driver", _DRIVER_KEYS)):
        where = f"driver[{i}]"
        name = _str(fields, "name")
        arches = fields.get("arches")
        if not name:
            errors.append(f"{where}.name: required (non-empty string)")
            continue
        if not isinstance(arches, tuple) or not arches:
            errors.append(f"{where}.arches: required (non-empty string list)")
            continue
        entries.append(DriverComponent(
            name=name,
            source=_str(fields, "source") or "",
            arches=arches,
            early_arches=_str_list(fields, "early_arches"),
            description=_str(fields, "description"),
        ))
    if errors:
        raise RegistryError(errors=tuple(errors))
    return tuple(entries)


def load_flash_backends(repo_root: Path) -> tuple[FlashBackend, ...]:
    """Parse the flash-backend registry TOML into typed backends or raise RegistryError."""
    path = flash_backends_path(repo_root)
    errors: list[str] = []
    entries: list[FlashBackend] = []
    for i, fields in enumerate(_parse_array_of_tables(path, "backend", _FLASH_BACKEND_KEYS)):
        where = f"backend[{i}]"
        name = _str(fields, "name")
        boards = fields.get("boards")
        target = _str(fields, "make_target")
        if not name:
            errors.append(f"{where}.name: required (non-empty string)")
            continue
        if not isinstance(boards, tuple) or not boards:
            errors.append(f"{where}.boards: required (non-empty string list)")
            continue
        if not target:
            errors.append(f"{where}.make_target: required (non-empty string)")
            continue
        flash_kb, ram_kb = fields.get("flash_kb"), fields.get("ram_kb")
        entries.append(FlashBackend(
            name=name,
            boards=boards,
            make_target=target,
            description=_str(fields, "description"),
            flash_kb=flash_kb if isinstance(flash_kb, int) else None,
            ram_kb=ram_kb if isinstance(ram_kb, int) else None,
        ))
    if errors:
        raise RegistryError(errors=tuple(errors))
    return tuple(entries)


def validate_registry(entries: tuple[DriverComponent, ...], repo_root: Path) -> list[str]:
    """Self-consistency: unique names, known arches, early subset, source files."""
    e: list[str] = []
    seen: set[str] = set()
    for d in entries:
        if d.name in seen:
            e.append(f"driver '{d.name}': duplicate entry")
        seen.add(d.name)
        for arch in d.arches:
            if arch not in KNOWN_ARCHES:
                e.append(f"driver '{d.name}': unknown arch '{arch}'")
        for arch in d.early_arches:
            if arch not in d.arches:
                e.append(f"driver '{d.name}': early_arches '{arch}' not in arches")
            elif arch not in GENERIC_DEPLOYMENT_ARCHES:
                e.append(f"driver '{d.name}': early_arches '{arch}' has no generic deployment")
        if not d.source:
            e.append(f"driver '{d.name}': source is required")
        elif not (repo_root / d.source).is_file():
            e.append(f"driver '{d.name}': source file missing: {d.source}")
    return e


def validate_driver_selection(inst: Instance, entries: tuple[DriverComponent, ...]) -> list[str]:
    """Check an instance's [rootfs].drivers against the registry."""
    if inst.rootfs.drivers is None:
        return []
    by_name = {d.name: d for d in entries}
    e: list[str] = []
    for name in inst.rootfs.drivers:
        d = by_name.get(name)
        if d is None:
            e.append(f"rootfs.drivers: unknown driver '{name}' (see components/drivers.toml)")
        elif inst.arch not in d.arches:
            e.append(f"rootfs.drivers: '{name}' does not support {inst.arch}")
        elif inst.arch in d.early_arches:
            e.append(f"rootfs.drivers: '{name}' is an early driver on {inst.arch} "
                     "(already embedded in the kernel image)")
    return e


def _make_var(repo_root: Path, arch: str, var: str) -> tuple[str, ...]:
    """Print one variable from the Makefile without parsing it ourselves."""
    out = subprocess.run(
        ["make", "-s", "-C", str(repo_root), f"ARCH={arch}", "DRIVER_DEPLOYMENT=generic",
         "--eval", f"print-a20-registry:;@echo $({var})", "print-a20-registry"],
        check=False, capture_output=True, text=True,
    )
    if out.returncode != 0:
        raise RegistryError(errors=(f"make failed for ARCH={arch}: {out.stderr.strip()}",))
    return tuple(out.stdout.split())


def cross_check_make(entries: tuple[DriverComponent, ...], repo_root: Path) -> list[str]:
    """Compare the registry against DRVMOD_MODULES/EARLY_DRVMOD_MODULES per arch."""
    e: list[str] = []
    for arch in GENERIC_DEPLOYMENT_ARCHES:
        make_modules = set(_make_var(repo_root, arch, "DRVMOD_MODULES"))
        make_early = set(_make_var(repo_root, arch, "EARLY_DRVMOD_MODULES"))
        reg_modules = {d.package for d in entries if arch in d.arches}
        reg_early = {d.package for d in entries if arch in d.early_arches}
        if make_modules != reg_modules:
            e.append(f"{arch}: DRVMOD_MODULES drift — "
                     f"only in Makefile: {sorted(make_modules - reg_modules)}, "
                     f"only in registry: {sorted(reg_modules - make_modules)}")
        if make_early != reg_early:
            e.append(f"{arch}: EARLY_DRVMOD_MODULES drift — "
                     f"only in Makefile: {sorted(make_early - reg_early)}, "
                     f"only in registry: {sorted(reg_early - make_early)}")
    return e


def validate_flash_backends(entries: tuple[FlashBackend, ...], repo_root: Path) -> list[str]:
    """Unique names, boards that exist as platform dirs, targets that exist as rules.

    The board check is what keeps the allowlist honest: a typo in `boards`
    would otherwise silently exclude a real board from programming rather than
    announcing itself.
    """
    e: list[str] = []
    seen: set[str] = set()
    makefiles = [repo_root / "Makefile", *sorted((repo_root / "tools").glob("*.mk"))]
    for b in entries:
        if b.name in seen:
            e.append(f"backend '{b.name}': duplicate entry")
        seen.add(b.name)
        for board in b.boards:
            if not (repo_root / "kernel" / "platform" / board / "board.c").is_file():
                e.append(f"backend '{b.name}': board '{board}' has no kernel/platform/{board}/board.c")
        rule = re.compile(rf"^{re.escape(b.make_target)}\s*:", re.MULTILINE)
        if not any(rule.search(f.read_text(encoding="utf-8", errors="replace")) for f in makefiles):
            e.append(f"backend '{b.name}': make target '{b.make_target}' is not defined in any makefile")
    return e


def cross_check_flash_targets(entries: tuple[FlashBackend, ...], repo_root: Path) -> list[str]:
    """Every make target reachable from a flash backend must be reachable from a20.

    Catches the inverse of the original bug: a backend whose target exists but
    which nothing dispatches to is dead configuration.
    """
    from a20_board import FLASH_TARGET_REACHABLE

    return [f"backend '{b.name}': make target '{b.make_target}' is not in FLASH_TARGET_REACHABLE"
            for b in entries if b.make_target not in FLASH_TARGET_REACHABLE]
