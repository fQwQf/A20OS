"""Component registries (components/*.toml).

Two registries live here and share one TOML table parser, because they answer
the same shape of question -- "what exists, and does the build agree?" -- over
different vocabularies.

`components/drivers.toml` declares which .a20drv driver packages exist, which
architectures each supports, and which are embedded early into the kernel root
ramfs.  It is the only place a driver is declared: the Makefile's per-arch
DRVMOD_MODULES / EARLY_DRVMOD_MODULES lists are generated from it into
`components/drivers.mk`, which make includes.  `a20 check-registry` fails if
that generated file is stale.

`components/flash-backends.toml` declares which ways of programming a board's
non-volatile memory exist, which boards each is validated for, and which make
target implements it.  `boards` is a safety allowlist, not documentation: a
manifest naming a board outside it is rejected before anything is built.

`components/trim.toml` declares the policy half of kernel trimming: the
source list each build profile curates, and which architectures each
optional capability supports.  The Makefile consumes the generated
`components/trim.mk` fragment; tools/a20 reads this TOML when validating
instance manifests, so instance validation and the build cannot drift apart.
"""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any, Final, Sequence

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


def make_fragment_path(repo_root: Path) -> Path:
    return repo_root / "components" / "drivers.mk"


def _modules_for(entries: tuple[DriverComponent, ...], arch: str,
                 *, early: bool) -> tuple[str, ...]:
    out: list[str] = []
    for d in entries:
        if arch not in d.arches:
            continue
        if early and arch not in d.early_arches:
            continue
        out.append(d.package)
    return tuple(out)


def render_make_fragment(entries: tuple[DriverComponent, ...],
                         arches: Sequence[str] = GENERIC_DEPLOYMENT_ARCHES) -> str:
    """Render the make fragment that carries the per-arch module lists.

    The lists used to be hand-maintained in tools/driver-modules.mk next to a
    cross-check that compared the two copies.  Generating the fragment removes
    the hand-maintained copy, so the TOML is the only place a driver is
    declared; `check-component-registry` now only has to prove this file is
    current, which costs one parse instead of eight make subprocesses.

    Make still reads plain assignments, so nothing here costs a parse-time
    $(shell) -- worth avoiding, since the recursive $(MAKE) in this build means
    a per-invocation cost is paid dozens of times over.
    """
    lines = [
        "# GENERATED from components/drivers.toml by `make regen-driver-fragment`.",
        "# Do not edit by hand: edit the TOML and regenerate.  `make",
        "# check-component-registry` fails if this file is stale.",
        "",
    ]
    for arch in arches:
        mods = " ".join(_modules_for(entries, arch, early=False))
        early = " ".join(_modules_for(entries, arch, early=True))
        lines.append(f"DRVMOD_MODULES_{arch} := {mods}")
        lines.append(f"EARLY_DRVMOD_MODULES_{arch} := {early}")
    lines += [
        "",
        "# Unset for any other ARCH, matching the previous explicit empty",
        "# assignment: undefined and empty are equivalent for every consumer.",
        "DRVMOD_MODULES := $(DRVMOD_MODULES_$(ARCH))",
        "EARLY_DRVMOD_MODULES := $(EARLY_DRVMOD_MODULES_$(ARCH))",
        "",
    ]
    return "\n".join(lines)


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


def check_make_fragment(entries: tuple[DriverComponent, ...],
                        repo_root: Path) -> list[str]:
    """Report if components/drivers.mk is stale with respect to the TOML.

    This replaces the old DRVMOD_MODULES/EARLY_DRVMOD_MODULES cross-check.
    That one compared two hand-maintained lists, which meant it could only ever
    tell you they had drifted -- it could not remove the second copy.  The lists
    are generated now, so the only question left is whether the checked-in
    fragment still matches its source, and that is a string comparison rather
    than eight make subprocesses (1.7s -> ~40ms).
    """
    want = render_make_fragment(entries)
    path = make_fragment_path(repo_root)
    try:
        have = path.read_text(encoding="utf-8")
    except OSError as exc:
        return [f"{path.name}: cannot read ({exc}); run `make regen-driver-fragment`"]
    if have == want:
        return []
    return [f"{path.name} is stale with respect to {registry_path(repo_root).name}; "
            "run `make regen-driver-fragment`"]


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


# ---------------------------------------------------------------------------
# Kernel trim registry (components/trim.toml)
# ---------------------------------------------------------------------------

_TRIM_PROFILE_KEYS: Final = {"description": "str", "arches": "str_list",
                             "opt": "str", "cppflags": "str_list",
                             "nommu": "bool", "bringup": "bool",
                             "sources": "str_list"}
_TRIM_CAPABILITY_KEYS: Final = {"description": "str", "arches": "str_list",
                                "board_exclusions": "str_list"}

# The Makefile's gates consume these unconditionally; a missing entry would
# silently turn the corresponding gate into a no-op (an empty arch list
# disables a capability everywhere), so their absence is a registry error
# rather than a build surprise.
REQUIRED_TRIM_CAPABILITIES: Final = ("nommu", "swap", "xlator", "ramfs-user",
                                     "smp-verified-qemu", "pcie-mmio-alloc",
                                     "pcie-mmio-ecam")
REQUIRED_TRIM_PROFILES: Final = ("mcu",)

# Capabilities whose build-side enforcement is a Makefile mechanism other than
# the emitted TRIM_CAP_* variable.  ramfs-user is gated on the
# RAMFS_USER_OBJCOPY_<arch> objcopy table -- deliberately, so adding an arch
# is one recipe line instead of two places that can disagree -- and this
# registry only carries the instance-facing policy.  cross_check_trim_consumers
# must not demand a makefile consumer these capabilities cannot have.
A20_VALIDATED_ONLY_CAPABILITIES: Final = ("ramfs-user",)


@dataclass(frozen=True, slots=True)
class TrimProfile:
    name: str
    arches: tuple[str, ...]
    opt: str | None
    cppflags: tuple[str, ...]
    sources: tuple[str, ...]
    nommu: bool
    bringup: bool
    description: str | None


@dataclass(frozen=True, slots=True)
class TrimCapability:
    name: str
    arches: tuple[str, ...]
    board_exclusions: tuple[str, ...]
    description: str | None


@dataclass(frozen=True, slots=True)
class TrimRegistry:
    profiles: dict[str, TrimProfile]
    capabilities: dict[str, TrimCapability]
    arch_cppflags: dict[str, tuple[str, ...]]


@dataclass(frozen=True, slots=True)
class TrimDecision:
    """One line of `a20 trim`: what the build will do and why."""

    label: str
    state: str
    detail: str


def _parse_named_tables(path: Path, section: str, spec: dict[str, str],
                        *, required: bool) -> dict[str, dict[str, Any]]:
    """Read one TOML table-of-tables into {name: typed fields}.

    Same accumulate-every-error stance as _parse_array_of_tables, but the
    entries are named tables ([capability.nommu]) rather than an array of
    tables, because a capability is looked up by name, not by position.
    """
    try:
        with path.open("rb") as f:
            raw = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        raise RegistryError(errors=(f"{path.name}: TOML syntax error: {e}",)) from None
    except OSError as e:
        raise RegistryError(errors=(f"{path.name}: {e}",)) from None

    table = raw.get(section)
    if table is None:
        if required:
            raise RegistryError(errors=(f"{path.name}: expected a [{section}] table",))
        return {}
    if not isinstance(table, dict):
        raise RegistryError(errors=(f"{path.name}: [{section}] must be a table of tables",))

    errors: list[str] = []
    out: dict[str, dict[str, Any]] = {}
    for name, item in table.items():
        where = f"{section}.{name}"
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
            elif kind == "bool":
                ok = isinstance(value, bool)
            else:
                ok = isinstance(value, list) and all(isinstance(v, str) for v in value)
            if ok:
                fields[key] = tuple(value) if kind == "str_list" else value
            else:
                errors.append(f"{where}.{key}: expected {kind}, got {type(value).__name__}")
        out[name] = fields
    if errors:
        raise RegistryError(errors=tuple(errors))
    return out


def _parse_str_list_map(path: Path, section: str) -> dict[str, tuple[str, ...]]:
    """Read one TOML table whose values are string lists ({arch: [flags]})."""
    try:
        with path.open("rb") as f:
            raw = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        raise RegistryError(errors=(f"{path.name}: TOML syntax error: {e}",)) from None
    except OSError as e:
        raise RegistryError(errors=(f"{path.name}: {e}",)) from None
    table = raw.get(section) or {}
    if not isinstance(table, dict):
        raise RegistryError(errors=(f"{path.name}: [{section}] must be a table",))
    errors: list[str] = []
    out: dict[str, tuple[str, ...]] = {}
    for key, value in table.items():
        if isinstance(value, list) and all(isinstance(v, str) for v in value):
            out[key] = tuple(value)
        else:
            errors.append(f"{section}.{key}: expected a string list")
    if errors:
        raise RegistryError(errors=tuple(errors))
    return out


def trim_registry_path(repo_root: Path) -> Path:
    return repo_root / "components" / "trim.toml"


def trim_fragment_path(repo_root: Path) -> Path:
    return repo_root / "components" / "trim.mk"


@lru_cache(maxsize=None)
def load_trim_registry(repo_root: Path) -> TrimRegistry:
    """Parse components/trim.toml into typed profiles/capabilities or raise.

    Cached because every instance validation now consults it: parsing a
    45-instance tree must not mean parsing this file 45 times.
    """
    path = trim_registry_path(repo_root)
    profiles: dict[str, TrimProfile] = {}
    capabilities: dict[str, TrimCapability] = {}
    arch_cppflags: dict[str, tuple[str, ...]] = {}
    for name, fields in _parse_named_tables(path, "profile", _TRIM_PROFILE_KEYS,
                                            required=True).items():
        profiles[name] = TrimProfile(
            name=name,
            arches=_str_list(fields, "arches"),
            opt=_str(fields, "opt"),
            cppflags=_str_list(fields, "cppflags"),
            sources=_str_list(fields, "sources"),
            nommu=fields.get("nommu", False),
            bringup=fields.get("bringup", False),
            description=_str(fields, "description"),
        )
    for name, fields in _parse_named_tables(path, "capability", _TRIM_CAPABILITY_KEYS,
                                            required=True).items():
        capabilities[name] = TrimCapability(
            name=name,
            arches=_str_list(fields, "arches"),
            board_exclusions=_str_list(fields, "board_exclusions"),
            description=_str(fields, "description"),
        )
    arch_cppflags = _parse_str_list_map(path, "arch-cppflags")
    return TrimRegistry(profiles=profiles, capabilities=capabilities,
                        arch_cppflags=arch_cppflags)


def validate_trim_registry(reg: TrimRegistry, repo_root: Path) -> list[str]:
    """Self-consistency: required entries, known arches, existing source files."""
    e: list[str] = []
    for name in REQUIRED_TRIM_PROFILES:
        if name not in reg.profiles:
            e.append(f"profile '{name}': required by the Makefile's gates but not declared")
    for name in REQUIRED_TRIM_CAPABILITIES:
        if name not in reg.capabilities:
            e.append(f"capability '{name}': required by the Makefile's gates but not declared")

    for p in reg.profiles.values():
        if not p.arches:
            e.append(f"profile '{p.name}': arches must be a non-empty string list")
        for arch in p.arches:
            if arch not in KNOWN_ARCHES:
                e.append(f"profile '{p.name}': unknown arch '{arch}'")
        if p.opt is not None and not p.opt.startswith("-"):
            e.append(f"profile '{p.name}': opt '{p.opt}' must look like a compiler flag")
        for flag in p.cppflags:
            if not flag.startswith("-"):
                e.append(f"profile '{p.name}': cppflags entry '{flag}' must start with '-'")
        seen: set[str] = set()
        for src in p.sources:
            if src in seen:
                e.append(f"profile '{p.name}': duplicate source '{src}'")
            seen.add(src)
            if not src.startswith("kernel/") or ".." in Path(src).parts:
                e.append(f"profile '{p.name}': source '{src}' must be a kernel/-relative path")
            elif not (repo_root / src).is_file():
                e.append(f"profile '{p.name}': source file missing: {src}")

    for c in reg.capabilities.values():
        for arch in c.arches:
            if arch not in KNOWN_ARCHES:
                e.append(f"capability '{c.name}': unknown arch '{arch}'")
        for board in c.board_exclusions:
            if not (repo_root / "kernel" / "platform" / board / "board.c").is_file():
                e.append(f"capability '{c.name}': board '{board}' has no "
                         f"kernel/platform/{board}/board.c")

    for arch in reg.arch_cppflags:
        if arch not in KNOWN_ARCHES:
            e.append(f"arch-cppflags: unknown arch '{arch}'")
        for flag in reg.arch_cppflags[arch]:
            if not flag.startswith("-"):
                e.append(f"arch-cppflags.{arch}: entry '{flag}' must start with '-'")
    return e


def _make_symbol(text: str) -> str:
    return text.replace("-", "_").upper()


def render_trim_fragment(reg: TrimRegistry) -> str:
    """Render the make fragment that carries the trim lists.

    Plain assignments only, same stance as render_make_fragment: the
    recursive $(MAKE) in this build means a per-invocation $(shell) cost is
    paid dozens of times over, so the TOML is parsed once by this tool and
    make reads text.
    """
    lines = [
        "# GENERATED from components/trim.toml by `make regen-trim-fragment`.",
        "# Do not edit by hand: edit the TOML and regenerate.  `make",
        "# check-trim-registry` fails if this file is stale.",
        "",
    ]
    for name in sorted(reg.profiles):
        p = reg.profiles[name]
        n = _make_symbol(name)
        lines.append(f"TRIM_PROFILE_{n}_ARCHES := {' '.join(p.arches)}")
        lines.append(f"TRIM_PROFILE_{n}_OPT := {p.opt or ''}")
        lines.append(f"TRIM_PROFILE_{n}_CPPFLAGS := {' '.join(p.cppflags)}")
        lines.append(f"TRIM_PROFILE_{n}_SOURCES := {' '.join(p.sources)}")
        if p.nommu:
            lines.append(f"TRIM_PROFILE_{n}_FORCE_NOMMU := 1")
        if p.bringup:
            lines.append(f"TRIM_PROFILE_{n}_FORCE_BRINGUP := 1")
        lines.append("")
    for name in sorted(reg.capabilities):
        c = reg.capabilities[name]
        n = _make_symbol(name)
        lines.append(f"TRIM_CAP_{n}_ARCHES := {' '.join(c.arches)}")
        lines.append(f"TRIM_CAP_{n}_BOARD_EXCLUSIONS := {' '.join(c.board_exclusions)}")
    if reg.arch_cppflags:
        lines.append("")
        for arch in sorted(reg.arch_cppflags):
            lines.append(f"TRIM_ARCH_CPPFLAGS_{arch} := {' '.join(reg.arch_cppflags[arch])}")
    lines.append("")
    return "\n".join(lines)


def check_trim_fragment(reg: TrimRegistry, repo_root: Path) -> list[str]:
    """Report if components/trim.mk is stale with respect to the TOML."""
    want = render_trim_fragment(reg)
    path = trim_fragment_path(repo_root)
    try:
        have = path.read_text(encoding="utf-8")
    except OSError as exc:
        return [f"{path.name}: cannot read ({exc}); run `make regen-trim-fragment`"]
    if have == want:
        return []
    return [f"{path.name} is stale with respect to {trim_registry_path(repo_root).name}; "
            "run `make regen-trim-fragment`"]


def cross_check_trim_consumers(reg: TrimRegistry, repo_root: Path) -> list[str]:
    """Every variable the fragment emits must have a makefile consumer.

    A registry entry nothing reads is dead policy: the gate would stay green
    while the knob it documents stopped doing anything.  This is the trim
    analogue of cross_check_flash_targets.
    """
    makefiles = [repo_root / "Makefile", *sorted((repo_root / "tools").glob("*.mk"))]
    joined = "\n".join(f.read_text(encoding="utf-8", errors="replace")
                       for f in makefiles if f.is_file())
    e: list[str] = []
    for name, p in sorted(reg.profiles.items()):
        n = _make_symbol(name)
        required = [f"TRIM_PROFILE_{n}_ARCHES", f"TRIM_PROFILE_{n}_OPT",
                    f"TRIM_PROFILE_{n}_CPPFLAGS", f"TRIM_PROFILE_{n}_SOURCES"]
        if p.nommu:
            required.append(f"TRIM_PROFILE_{n}_FORCE_NOMMU")
        if p.bringup:
            required.append(f"TRIM_PROFILE_{n}_FORCE_BRINGUP")
        for var in required:
            if var not in joined:
                e.append(f"profile '{name}': no makefile reads {var}")
    for name in sorted(reg.capabilities):
        if name in A20_VALIDATED_ONLY_CAPABILITIES:
            continue
        n = _make_symbol(name)
        if f"TRIM_CAP_{n}_ARCHES" not in joined:
            e.append(f"capability '{name}': no makefile reads TRIM_CAP_{n}_ARCHES")
    if reg.arch_cppflags and "TRIM_ARCH_CPPFLAGS_$" not in joined:
        e.append("arch-cppflags: no makefile reads TRIM_ARCH_CPPFLAGS_$")
    return e


def plan_trim(inst: Instance, reg: TrimRegistry) -> list[TrimDecision]:
    """Derive the effective trim an instance's build will follow.

    Mirrors the Makefile's own gate order (profile implications first, then
    the NOMMU-driven force-offs, then the arch matrices) so the report agrees
    with what make will actually do.  The Makefile stays authoritative; this
    is a reading of it, keyed on the same registry the build consumes.
    """
    d: list[TrimDecision] = []
    k = inst.kernel

    mcu = reg.profiles.get("mcu")
    mcu_arches = mcu.arches if mcu else ()
    profile_forced = inst.arch in mcu_arches
    if profile_forced:
        profile_name = "mcu"
        detail = f"implied by arch {inst.arch} ([profile.mcu].arches)"
    else:
        profile_name = k.profile or "full"
        detail = "instance default" if k.profile is None else "requested by kernel.profile"
    profile = reg.profiles.get(profile_name)
    detail_tail = (f"; {len(profile.sources)} curated sources + board/platform/arch files"
                   if profile else "; no curated source list")
    d.append(TrimDecision("profile", profile_name, detail + detail_tail
                          + (f"; {profile.opt} {' '.join(profile.cppflags)}"
                             if profile and profile.opt else "")))

    nommu = bool(profile_forced and profile and profile.nommu) or bool(k.nommu)
    nommu_arches = reg.capabilities["nommu"].arches if "nommu" in reg.capabilities else ()
    if profile_forced and profile and profile.nommu:
        d.append(TrimDecision("nommu", "on", "forced by profile mcu; NOMMU=1"))
    elif nommu:
        d.append(TrimDecision("nommu", "on", "requested by kernel.nommu"))
    else:
        d.append(TrimDecision("nommu", "off", f"supported on: {', '.join(nommu_arches)}"))

    # Makefile gate order: NOMMU=1 forces CONFIG_SWAP=n and CONFIG_XLATOR=n
    # before the arch matrices are consulted.
    swap_arches = reg.capabilities["swap"].arches if "swap" in reg.capabilities else ()
    swap_requested = k.swap if k.swap is not None else True   # CONFIG_SWAP ?= y
    if nommu:
        d.append(TrimDecision("swap", "off", "NOMMU=1 leaves nothing to demand-page from"))
    elif inst.arch not in swap_arches:
        d.append(TrimDecision("swap", "off", f"unsupported for {inst.arch}"))
    else:
        d.append(TrimDecision("swap", "on" if swap_requested else "off",
                              "on by default; CONFIG_SWAP ?= y"))
    xlator_arches = reg.capabilities["xlator"].arches if "xlator" in reg.capabilities else ()
    if nommu and profile_forced:
        d.append(TrimDecision("xlator", "off", "NOMMU=1 / PROFILE=mcu replaces the source list"))
    elif profile_forced:
        d.append(TrimDecision("xlator", "off", "PROFILE=mcu replaces the source list"))
    elif nommu:
        d.append(TrimDecision("xlator", "off", "NOMMU=1 leaves no MMU to demand-page guest memory from"))
    elif inst.arch not in xlator_arches:
        d.append(TrimDecision("xlator", "off", f"unsupported for {inst.arch}"))
    else:
        d.append(TrimDecision("xlator", "on", "on by default; CONFIG_XLATOR ?= y"))

    ramfs_arches = reg.capabilities["ramfs-user"].arches if "ramfs-user" in reg.capabilities else ()
    ramfs_on = bool(k.ramfs_user)
    if ramfs_on and inst.arch not in ramfs_arches:
        d.append(TrimDecision("ramfs_user", "unsupported",
                              f"supported on: {', '.join(ramfs_arches)}"))
    else:
        d.append(TrimDecision("ramfs_user", "on" if ramfs_on else "off",
                              f"supported on: {', '.join(ramfs_arches)}"))

    smp_arches = (reg.capabilities["smp-verified-qemu"].arches
                  if "smp-verified-qemu" in reg.capabilities else ())
    smp = inst.machine.smp or 1
    verified = (inst.arch in smp_arches
                and inst.board == f"qemu-virt-{inst.arch}")
    d.append(TrimDecision("smp", f"{smp} cpu",
                          "pre-verified platform" if smp == 1 or verified
                          else f"unverified for {inst.arch}/{inst.board}; needs "
                               "machine.allow_unverified_smp = true"))

    if inst.stm32.flash_kb is not None or inst.stm32.ram_kb is not None:
        flash = f"{inst.stm32.flash_kb} KiB" if inst.stm32.flash_kb is not None else "?"
        ram = f"{inst.stm32.ram_kb} KiB" if inst.stm32.ram_kb is not None else "?"
        d.append(TrimDecision("memory", f"flash {flash}, ram {ram}",
                              "linked via FLASH_LENGTH/RAM_LENGTH defsym; the link fails "
                              "when the image overflows"))
    elif inst.machine.memory is not None:
        d.append(TrimDecision("memory", f"QEMU -m {inst.machine.memory}", "guest memory"))
    return d
