#!/usr/bin/env python3
"""Package/image orchestration that used to live in tools/targets-pkg.mk.

The Makefile owned three things that are not compilation: iterating the recipe
list, deciding which optional arguments mka20pkg/mkrootfs get, and sequencing
package -> repo -> image.  All of it is now here, so the make targets are one
line each and make is left with building.

Every value the makefile used to compute is still asked of make, never
re-derived here -- the same rule a20_make.query_make documents.  `--from-make`
prints them; the makefile passes them in.
"""

from __future__ import annotations

import argparse
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

from mkrootfs import ELF_MACHINES, elf_machine

REPO = Path(__file__).resolve().parent.parent


def _media_ignore(src: Path, arch: str, dropped: list[str]):
    """copytree ignore callback dropping ELFs built for another architecture.

    Media is data, except the Mojang natives/: they are ELF, Mojang publishes
    them for x86_64 (arm64 on newer versions) and never for riscv64, and
    mkrootfs' overlay check would (rightly) refuse an image that ships them.
    Dropping them here keeps the data (jars, assets) -- which is
    arch-independent -- and turns a build failure into a log line.  With no
    known --arch nothing is filtered.
    """
    want = ELF_MACHINES.get(arch)
    if want is None:
        return None

    def ignore(directory: str, names: list[str]) -> list[str]:
        skip: list[str] = []
        for n in names:
            p = Path(directory) / n
            if p.is_symlink() or not p.is_file():
                continue
            machine = elf_machine(p)
            if machine is not None and machine != want:
                dropped.append(str(Path(directory).relative_to(src) / n))
                skip.append(n)
        return skip

    return ignore


def run(cmd: list[str], **kw) -> None:
    print(f"+ {shlex.join(cmd)}", flush=True)
    r = subprocess.run(cmd, cwd=REPO, check=False, **kw)
    if r.returncode != 0:
        raise SystemExit(r.returncode)


def recipes(a) -> list[str]:
    out = []
    for name in a.recipes.split():
        p = REPO / "packages" / "recipes" / f"{name}.toml"
        if not p.exists():
            raise SystemExit(f"[PKG] unknown recipe: {name}")
        out.append(str(p))
    return out


def cmd_pkg_key(a) -> int:
    """Local development signing key: generated here, never committed.

    The release key comes from a CI secret instead (docs/packaging/repository.md).
    """
    key = REPO / a.sign_key
    if key.exists():
        return 0
    key.parent.mkdir(parents=True, exist_ok=True)
    run(["openssl", "genrsa", "-out", str(key), "2048"])
    run(["openssl", "rsa", "-in", str(key), "-pubout",
         "-out", str(REPO / a.keys_dir / a.key_name)])
    print(f"[PKG] generated development signing key: {a.sign_key}")
    return 0


def cmd_pkgs_check(a) -> int:
    """Validate recipes and artifact paths without writing packages."""
    for r in recipes(a):
        run([sys.executable, "tools/mka20pkg.py", r, "--arch", a.arch,
             "--variant", a.variant, "--kernel-build-dir", a.build_dir, "--check"])
    return 0


def cmd_pkgs(a) -> int:
    for r in recipes(a):
        cmd = [sys.executable, "tools/mka20pkg.py", r, "--arch", a.arch,
               "--variant", a.variant, "--kernel-build-dir", a.build_dir]
        if a.sign_key:
            cmd += ["--sign-key", str((REPO / a.sign_key).resolve()),
                    "--key-name", a.key_name]
        cmd += ["-o", a.out_dir]
        run(cmd)
    return 0


def cmd_pkg_repo(a) -> int:
    dest = Path(a.repo_dir) / a.arch
    dest.mkdir(parents=True, exist_ok=True)
    for apk in sorted(Path(a.out_dir).glob("*.apk")):
        shutil.copy2(apk, dest)
    cmd = ["tools/mka20repo.sh"]
    if a.sign_key:
        cmd += ["--sign-key", a.sign_key, "--key-name", a.key_name]
    run(cmd + [str(dest)])
    return 0


def cmd_media_overlay(a) -> int:
    """Stage GUI media into a throwaway overlay that mkrootfs merges."""
    ov = Path(a.media_overlay)
    if ov.exists():
        shutil.rmtree(ov)
    (ov / a.media_dir.lstrip("/")).mkdir(parents=True, exist_ok=True)
    (ov / "root" / "Desktop").mkdir(parents=True, exist_ok=True)
    for m in a.media.split():
        src = Path(m)
        if not src.exists():
            raise SystemExit(f"[PKG] GUI_MEDIA not found: {m}")
        dst = ov / a.media_dir.lstrip("/") / src.name
        want = ELF_MACHINES.get(a.arch)
        dropped: list[str] = []
        if src.is_dir():
            shutil.copytree(src, dst, dirs_exist_ok=True,
                            ignore=_media_ignore(src, a.arch, dropped))
            # copytree materialises every directory it walks, so one whose
            # files were all dropped above survives as an empty husk.  Remove
            # it: an empty natives/ would let the guest launcher sail past its
            # own presence check and die later in dlopen with no explanation.
            for d in sorted(dst.rglob("*"), reverse=True):
                if d.is_dir() and not any(d.iterdir()):
                    d.rmdir()
        elif want is not None and elf_machine(src) not in (None, want):
            # A single-file media entry for another architecture: same rule as
            # a directory, just without the copytree traversal.
            dropped.append(src.name)
        else:
            shutil.copy2(src, dst)
        if dropped:
            shown = ", ".join(dropped[:3])
            if len(dropped) > 3:
                shown += f", ... ({len(dropped)} files)"
            print(f"[PKG] media: dropped {len(dropped)} ELF file(s) built for "
                  f"another architecture (target {a.arch or 'unspecified'}): "
                  f"{shown}", flush=True)
        print(f"[PKG] media: {m} -> {a.media_dir}/{src.name}")
    link = ov / "root" / "Desktop" / "a20-media"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(a.media_dir)
    return 0


def cmd_image_world(a) -> int:
    cmd = [sys.executable, "tools/mkrootfs.py", "--arch", a.arch,
           "--world", f"packages/world/{a.world}.world",
           "--repo", str((REPO / a.repo_dir).resolve())]
    ov = REPO / "packages" / "overlay" / a.world
    if ov.is_dir():
        cmd += ["--overlay", str(ov)]
    if a.media:
        cmd += ["--overlay", str((REPO / a.media_overlay).resolve())]
    cmd += ["--keys-dir", a.keys_dir] if a.sign_key else ["--allow-untrusted"]
    if a.alpine == "0":
        cmd += ["--no-alpine"]
    for pk in (a.ssh_pubkey.split() if a.ssh_pubkey else []):
        cmd += ["--ssh-pubkey", pk]
    if hasattr(__import__("os"), "getuid") and __import__("os").getuid() != 0:
        cmd += ["--usermode"]
    cmd += ["--output", f"{a.image_dir}/{a.world}-{a.arch}.img",
            "--size-mb", a.size_mb]
    run(cmd)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command",
                    choices=["pkg-key", "pkgs-check", "pkgs", "pkg-repo",
                             "media-overlay", "image-world"])
    # Values the Makefile owns; passed in rather than re-derived.
    for f, d in (("recipes", "space-separated recipe names"),
                 ("arch", "target arch"), ("variant", "user build variant"),
                 ("build-dir", "kernel build dir"), ("out-dir", "package out dir"),
                 ("repo-dir", "local repo dir"), ("image-dir", "image out dir"),
                 ("keys-dir", "key dir"), ("sign-key", "signing key path"),
                 ("key-name", "public key name"), ("world", "world name"),
                 ("size-mb", "image size in MiB"), ("alpine", "1 or 0"),
                 ("media", "GUI_MEDIA paths"), ("media-dir", "media dir in image"),
                 ("media-overlay", "media overlay dir"),
                 ("ssh-pubkey", "space-separated public key files")):
        ap.add_argument(f"--{f}", default="")
    a = ap.parse_args()
    # Root detection: a leading 0 means "we are root", so usermode is off.
    import os
    if a.alpine == "":
        a.alpine = "0" if os.getuid() == 0 else "1"
    return {
        "pkg-key": cmd_pkg_key, "pkgs-check": cmd_pkgs_check, "pkgs": cmd_pkgs,
        "pkg-repo": cmd_pkg_repo, "media-overlay": cmd_media_overlay,
        "image-world": cmd_image_world,
    }[a.command](a)


if __name__ == "__main__":
    raise SystemExit(main())
