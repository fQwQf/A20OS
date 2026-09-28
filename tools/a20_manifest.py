"""Artifact ledger for a20 instances (tools/a20 ledger).

The board bring-up records in docs/platforms/ used to be prose: an image size
and a SHA-256 copied by hand out of a terminal, dated, alongside a path under
one contributor's home directory.  Nothing about that is checkable, and a
changed byte is invisible unless someone re-reads the paragraph.

A ledger replaces the prose with a derived fact.  For one instance it reports
the build directory, the source revision, the variables the instance resolved
to, and every artifact with its size and SHA-256 -- all paths repository
relative, so the file is portable and diffable.  Regenerating it is cheaper
than retyping it, which is the only way a hand-maintained number stays true.

Artifact paths are obtained by asking make rather than by re-deriving
BUILD_DIR, whose name encodes a dozen build switches.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Final, Sequence

from a20_instance import Instance
from a20_make import REPO_ROOT, query_make

_CHUNK: Final = 1 << 20
_MAKE_VARS: Final = ("BUILD_DIR", "KERNEL_ELF", "KERNEL_BIN", "FAT32_IMG",
                     "EXT4_IMG", "EXTRA_IMG", "PKG_IMAGE_DIR", "PKG_ARCH")

# Firmware and boot-chain blobs live outside BUILD_DIR because they are built
# by their own targets (make vf2-firmware) and consumed by the image targets.
_BOOT_CHAIN: Final = (
    ("opensbi-fw_dynamic", "build/vf2-firmware/fw_dynamic.bin"),
    ("u-boot-spl", "build/vf2-firmware/u-boot-spl.bin.normal.out"),
    ("u-boot-itb", "build/vf2-firmware/u-boot.itb"),
    ("fit-image", "build/vf2-firmware/a20os.itb"),
    ("sd-card", "build/vf2-firmware/a20os-sd.img"),
)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


@dataclass(frozen=True, slots=True)
class Artifact:
    role: str
    path: str
    size: int
    sha256: str


@dataclass(frozen=True, slots=True)
class GitState:
    head: str
    branch: str
    dirty: bool


@dataclass(frozen=True, slots=True)
class Manifest:
    instance: str
    arch: str
    board: str
    abi: str | None
    build_dir: str
    git: GitState
    variables: dict[str, str]
    artifacts: tuple[Artifact, ...]
    missing: tuple[str, ...] = field(default=())


def git_state(repo_root: Path) -> GitState:
    def run(*args: str) -> str:
        out = subprocess.run(["git", *args], cwd=repo_root, check=False,
                             capture_output=True, text=True)
        return out.stdout.strip()

    status = run("status", "--porcelain")
    return GitState(
        head=run("rev-parse", "--short=12", "HEAD") or "(no commits)",
        branch=run("rev-parse", "--abbrev-ref", "HEAD") or "(detached)",
        dirty=bool(status),
    )


def _artifact(repo_root: Path, role: str, rel: str) -> Artifact | str:
    """Return an Artifact, or the path as a string when the file is absent."""
    p = repo_root / rel
    if not p.is_file():
        return rel
    return Artifact(role=role, path=rel, size=p.stat().st_size, sha256=sha256_file(p))


def _candidates(inst: Instance, vars_: dict[str, str]) -> list[tuple[str, str]]:
    """(role, repo-relative path) pairs worth reporting for this instance."""
    out: list[tuple[str, str]] = [("kernel-elf", vars_["KERNEL_ELF"]),
                                  ("kernel-bin", vars_["KERNEL_BIN"])]
    if inst.kernel.bringup is not True and inst.arch != "armv7m":
        out += [("rootfs-fat32", vars_["FAT32_IMG"]),
                ("rootfs-ext4", vars_["EXT4_IMG"])]
        if vars_["EXTRA_IMG"]:
            out.append(("extra-ext4", vars_["EXTRA_IMG"]))
    if inst.rootfs.world:
        out.append((f"world-{inst.rootfs.world}",
                    f"{vars_['PKG_IMAGE_DIR']}/{inst.rootfs.world}-{vars_['PKG_ARCH']}.img"))
    if inst.board == "visionfive2" or inst.package.kind == "fit-sdcard":
        out += _BOOT_CHAIN
    return [(role, rel) for role, rel in out if rel]


def collect(inst: Instance, repo_root: Path = REPO_ROOT) -> Manifest:
    vars_ = query_make(inst, _MAKE_VARS)
    artifacts: list[Artifact] = []
    missing: list[str] = []
    for role, rel in _candidates(inst, vars_):
        got = _artifact(repo_root, role, rel)
        if isinstance(got, Artifact):
            artifacts.append(got)
        else:
            missing.append(rel)
    return Manifest(
        instance=inst.name,
        arch=inst.arch,
        board=inst.board,
        abi=inst.abi,
        build_dir=vars_["BUILD_DIR"],
        git=git_state(repo_root),
        variables={k: v for k, v in vars_.items() if v},
        artifacts=tuple(artifacts),
        missing=tuple(missing),
    )


def _human(size: int) -> str:
    v = float(size)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if v < 1024 or unit == "GiB":
            return f"{v:.0f} {unit}" if unit == "B" else f"{v:.1f} {unit}"
        v /= 1024
    return f"{size} B"


def render_table(m: Manifest) -> str:
    lines = [f"instance : {m.instance}",
             f"arch     : {m.arch}   board: {m.board}   abi: {m.abi or '(makefile default)'}",
             f"build    : {m.build_dir}",
             f"git      : {m.git.head} on {m.git.branch}{' (dirty)' if m.git.dirty else ''}",
             "",
             f"{'role':<16} {'size':>10}  {'sha256':<64} path"]
    for a in m.artifacts:
        lines.append(f"{a.role:<16} {_human(a.size):>10}  {a.sha256}  {a.path}")
    if not m.artifacts:
        lines.append("(no artifacts present -- build the instance first)")
    if m.missing:
        lines += ["", "not built yet:"]
        lines += [f"  {p}" for p in m.missing]
    return "\n".join(lines)


def render_json(m: Manifest) -> str:
    payload = asdict(m)
    payload["artifacts"] = [asdict(a) for a in m.artifacts]
    payload["git"] = asdict(m.git)
    return json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def render_markdown(m: Manifest) -> str:
    """A fenced block suitable for pasting into a platform document."""
    body = [f"<!-- generated by: tools/a20 ledger {m.instance} -->",
            f"- revision: `{m.git.head}` ({m.git.branch}"
            f"{', dirty' if m.git.dirty else ''})",
            f"- build dir: `{m.build_dir}`",
            "",
            "| artifact | size | sha256 |", "| --- | ---: | --- |"]
    body += [f"| `{a.path}` | {_human(a.size)} | `{a.sha256}` |" for a in m.artifacts]
    return "\n".join(body) + "\n"


def write(m: Manifest, out: Path, fmt: str) -> None:
    text = {"json": render_json, "markdown": render_markdown}.get(fmt, render_table)(m)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")


def formats() -> Sequence[str]:
    return ("table", "json", "markdown")
