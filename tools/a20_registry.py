"""Loadable-driver component registry (components/drivers.toml).

The registry declares which .a20drv driver packages exist, which
architectures each supports, and which are embedded early into the kernel
root ramfs.  `a20 check-registry` validates the file itself and cross-checks
it against the Makefile's own build lists (DRVMOD_MODULES /
EARLY_DRVMOD_MODULES), so the registry and the build can never drift apart.
"""

from __future__ import annotations

import subprocess
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from a20_instance import KNOWN_ARCHES, Instance

# Architectures where DRIVER_DEPLOYMENT=generic (loadable modules) is supported
# and tools/driver-modules.mk defines per-arch module lists.
GENERIC_DEPLOYMENT_ARCHES: Final = ("riscv64", "x86_64", "aarch64", "loongarch64")

_REGISTRY_KEYS: Final = {"name": "str", "source": "str", "description": "str",
                         "arches": "str_list", "early_arches": "str_list"}


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
class RegistryError(Exception):
    """Structural parse failure of the registry; carries every error found."""

    errors: tuple[str, ...]

    def __str__(self) -> str:
        return "\n".join(f"  - {e}" for e in self.errors)


def registry_path(repo_root: Path) -> Path:
    return repo_root / "components" / "drivers.toml"


def load_registry(repo_root: Path) -> tuple[DriverComponent, ...]:
    """Parse the registry TOML into typed components or raise RegistryError."""
    path = registry_path(repo_root)
    try:
        with path.open("rb") as f:
            raw = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        raise RegistryError(errors=(f"{path.name}: TOML syntax error: {e}",)) from None
    except OSError as e:
        raise RegistryError(errors=(f"{path.name}: {e}",)) from None

    errors: list[str] = []
    entries: list[DriverComponent] = []
    table = raw.get("driver")
    if not isinstance(table, list):
        raise RegistryError(errors=(f"{path.name}: expected a [[driver]] array of tables",))
    for i, item in enumerate(table):
        where = f"driver[{i}]"
        if not isinstance(item, dict):
            errors.append(f"{where}: expected a table")
            continue
        fields: dict[str, object] = {}  # noqa: OBJECT_OK -- TOML boundary scratch
        for key, value in item.items():
            kind = _REGISTRY_KEYS.get(key)
            if kind is None:
                errors.append(f"{where}: unknown key '{key}'")
                continue
            ok = (isinstance(value, str) if kind == "str"
                  else isinstance(value, list) and all(isinstance(v, str) for v in value))
            if ok:
                fields[key] = tuple(value) if kind == "str_list" else value
            else:
                errors.append(f"{where}.{key}: expected {kind}, got {type(value).__name__}")
        name = fields.get("name")
        arches = fields.get("arches")
        if not isinstance(name, str) or not name:
            errors.append(f"{where}.name: required (string)")
            continue
        if not isinstance(arches, tuple) or not arches:
            errors.append(f"{where}.arches: required (non-empty string list)")
            continue
        early = fields.get("early_arches")
        entries.append(DriverComponent(
            name=name,
            source=fields.get("source") if isinstance(fields.get("source"), str) else "",
            arches=arches,
            early_arches=early if isinstance(early, tuple) else (),
            description=fields.get("description") if isinstance(fields.get("description"), str) else None,
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
