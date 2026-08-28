#!/usr/bin/env python3
"""mka20pkg — package A20OS build artifacts as a valid apk (v2) package.

Reads a TOML recipe (see packages/recipes/*.toml and
docs/packaging/packages.md), resolves its file list against the build tree,
and writes an apk v2 package:

    <outdir>/<name>-<version>-r<release>.apk

Package layout (gzip-segmented tar, verified against apk-tools 3.0):

    [signature stream]   only with --sign-key: tar containing
                         .SIGN.RSA256.<key-name> — an RSA/SHA-256
                         signature over the control gzip stream (the data
                         stream is authenticated by .PKGINFO's datahash)
    control stream       tar containing only .PKGINFO
    data stream          tar containing the payload; regular files carry
                         APK-TOOLS.checksum.SHA1 pax headers

apk's tar parser stops at the first end-of-archive marker and rejects any
non-zero data after it, so the signature/control streams MUST NOT contain
the conventional two-zero-block tar terminator (abuild behaves the same);
this module strips trailing zero blocks from those streams.  The data
stream keeps the standard terminator.

Unsigned (two-segment) packages install with `apk add --allow-untrusted`;
signed packages additionally verify with `--keys-dir`.

Placeholders available in recipe fields:
    {arch}         target apk architecture (e.g. riscv64)
    {variant}      user build variant (arch, or arch-nommu for NOMMU)
    {build_dir}    userland build output dir (user/build/<variant>)
    {extra_dir}    extra package build output dir (user/build/extra/<arch>)
    {kernel_build} kernel build dir (.kernel-build/<arch>)
    {name}         package name from [package]
    {version}      package version from [package]
    {repo}         repository root
    plus any keys from the recipe's [vars] table and --set name=value.
"""

from __future__ import annotations

import argparse
import glob
import gzip
import hashlib
import io
import os
import stat
import subprocess
import sys
import tarfile
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

CONTROL_NAME = ".PKGINFO"
DATA_HASH_KEY = "APK-TOOLS.checksum.SHA1"

REQUIRED_PACKAGE_KEYS = ("name", "version", "description", "license")


@dataclass
class FileEntry:
    """One payload member resolved from a [[files]] rule."""

    dest: str            # absolute in-package path, e.g. /sbin/init
    kind: str            # "file" | "symlink" | "inline"
    src: Path | None = None
    content: bytes | None = None
    link_target: str | None = None
    mode: int | None = None


@dataclass
class Recipe:
    path: Path
    package: dict
    files: list[dict] = field(default_factory=list)
    vars: dict = field(default_factory=dict)


def die(msg: str) -> "SystemExit":
    print(f"mka20pkg: error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def load_recipe(path: Path) -> Recipe:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        die(f"cannot parse recipe {path}: {exc}")
    package = data.get("package")
    if not isinstance(package, dict):
        die(f"recipe {path}: missing [package] table")
    for key in REQUIRED_PACKAGE_KEYS:
        if key not in package:
            die(f"recipe {path}: [package] missing required key '{key}'")
    files = data.get("files", [])
    if not isinstance(files, list):
        die(f"recipe {path}: [[files]] must be an array of tables")
    vars_ = data.get("vars", {})
    if not isinstance(vars_, dict):
        die(f"recipe {path}: [vars] must be a table")
    return Recipe(path=path, package=package, files=files, vars=vars_)


class Placeholders(dict):
    """format_map helper with a readable error for unknown placeholders."""

    def __missing__(self, key: str) -> str:
        raise KeyError(f"unknown placeholder '{{{key}}}'")


def substitute(text: str, ph: Placeholders, what: str) -> str:
    try:
        return text.format_map(ph)
    except KeyError as exc:
        die(f"{what}: {exc}")


def parse_mode(value: object, what: str) -> int:
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 8)
        except ValueError:
            pass
    die(f"{what}: invalid mode {value!r} (use an octal string like \"0755\")")


def resolve_files(recipe: Recipe, ph: Placeholders) -> list[FileEntry]:
    entries: list[FileEntry] = []
    for i, rule in enumerate(recipe.files):
        what = f"{recipe.path.name}: [[files]] #{i + 1}"
        dest_raw = rule.get("dest")
        if not dest_raw or not isinstance(dest_raw, str):
            die(f"{what}: missing 'dest'")
        dest_raw = substitute(dest_raw, ph, what)
        if not dest_raw.startswith("/"):
            die(f"{what}: dest must be an absolute in-package path, got {dest_raw!r}")
        mode = parse_mode(rule["mode"], what) if "mode" in rule else None

        if "symlink" in rule:
            target = substitute(str(rule["symlink"]), ph, what)
            entries.append(FileEntry(dest=dest_raw, kind="symlink",
                                     link_target=target, mode=mode))
            continue

        if "content" in rule:
            content = substitute(str(rule["content"]), ph, what).encode()
            entries.append(FileEntry(dest=dest_raw, kind="inline",
                                     content=content, mode=mode))
            continue

        src = rule.get("src")
        if not src or not isinstance(src, str):
            die(f"{what}: need one of 'src', 'content' or 'symlink'")
        src = substitute(src, ph, what)
        recursive = "**" in src
        matches = [m for m in sorted(glob.glob(src, recursive=recursive))]
        excludes = rule.get("exclude", [])
        if excludes:
            import fnmatch
            matches = [m for m in matches
                       if not any(fnmatch.fnmatch(Path(m).name, pat)
                                  for pat in excludes)]
        optional = bool(rule.get("optional", False))
        if not matches:
            if optional:
                continue
            die(f"{what}: src pattern matched nothing: {src}")
        dest_is_dir = dest_raw.endswith("/")
        # For a recursive glob, preserve the directory structure below the
        # part of the pattern preceding the first "**".
        rel_base: str | None = None
        if recursive:
            rel_base = src.split("**")[0].rstrip("/")
        files = []
        for m in matches:
            p = Path(m)
            if p.is_dir():
                # Mirrors the traditional image rules (`[ -f $f ] || continue`):
                # a plain non-recursive glob packages files only.
                continue
            files.append(p)
        if not files:
            if optional:
                continue
            die(f"{what}: src pattern matched only directories: {src}")
        if len(files) > 1 and not dest_is_dir:
            die(f"{what}: src matched {len(files)} files but dest "
                f"{dest_raw!r} is not a directory (end it with '/')")
        for p in files:
            if rel_base is not None:
                rel = os.path.relpath(p, rel_base)
                dest = dest_raw + rel
            else:
                dest = dest_raw + p.name if dest_is_dir else dest_raw
            entries.append(FileEntry(dest=dest, kind="file", src=p, mode=mode))
    return entries


def pkginfo_text(pkg: dict, arch: str, entries: list[FileEntry],
                 builddate: int, datahash: str) -> str:
    version = f"{pkg['version']}-r{int(pkg.get('release', 0))}"
    size = 0
    for e in entries:
        if e.kind == "file" and e.src is not None:
            size += e.src.stat().st_size
        elif e.kind == "inline" and e.content is not None:
            size += len(e.content)

    lines = [
        f"pkgname = {pkg['name']}",
        f"pkgver = {version}",
        f"arch = {arch}",
        f"size = {size}",
        f"pkgdesc = {pkg['description']}",
        f"url = {pkg.get('url', '')}",
        f"builddate = {builddate}",
        f"maintainer = {pkg.get('maintainer', '')}",
        f"origin = {pkg.get('origin', pkg['name'])}",
        f"license = {pkg['license']}",
        # SHA-256 over the raw data gzip stream; apk-tools 3.x refuses to
        # read a package whose .PKGINFO lacks it (APK_SIGN_VERIFY_AND_GENERATE
        # path in extract_v2.c).
        f"datahash = {datahash}",
    ]
    for dep in pkg.get("depends", []):
        lines.append(f"depend = {dep}")
    for prov in pkg.get("provides", []):
        lines.append(f"provides = {prov}")
    return "\n".join(lines) + "\n"


def _tarinfo(name: str, epoch: int) -> tarfile.TarInfo:
    ti = tarfile.TarInfo(name)
    ti.mtime = epoch
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    return ti


def strip_tar_trailer(data: bytes) -> bytes:
    """Remove trailing zero blocks so no end-of-archive marker remains.

    apk's tar parser stops at two consecutive zero blocks and rejects any
    non-zero block after them, so segments followed by more streams (the
    signature and control segments) must end exactly after their last entry.
    """
    end = len(data)
    while end >= 512 and data[end - 512:end] == b"\0" * 512:
        end -= 512
    return data[:end]


def build_control_tar(pkginfo: str, epoch: int) -> bytes:
    """Control segment: a tar with a single .PKGINFO entry."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        data = pkginfo.encode()
        ti = _tarinfo(CONTROL_NAME, epoch)
        ti.mode = 0o644
        ti.size = len(data)
        tar.addfile(ti, io.BytesIO(data))
    return strip_tar_trailer(buf.getvalue())


def build_data_tar(entries: list[FileEntry], epoch: int) -> bytes:
    """Data segment: payload tar, keeping the standard end-of-archive marker."""
    dirs: set[str] = set()
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:

        def ensure_dirs(dest: str) -> None:
            parts = dest.strip("/").split("/")[:-1]
            for i in range(1, len(parts) + 1):
                d = "/".join(parts[:i])
                if d not in dirs:
                    dirs.add(d)
                    di = _tarinfo(d, epoch)
                    di.type = tarfile.DIRTYPE
                    di.mode = 0o755
                    tar.addfile(di)

        for e in entries:
            name = e.dest.lstrip("/")
            ensure_dirs(name)
            if e.kind == "symlink":
                ti = _tarinfo(name, epoch)
                ti.type = tarfile.SYMTYPE
                ti.linkname = e.link_target or ""
                ti.mode = e.mode if e.mode is not None else 0o777
                tar.addfile(ti)
                continue
            if e.kind == "inline":
                data = e.content or b""
            else:
                assert e.src is not None
                st = e.src.stat()
                if not stat.S_ISREG(st.st_mode):
                    die(f"{e.src}: not a regular file")
                data = e.src.read_bytes()
            ti = _tarinfo(name, epoch)
            if e.mode is not None:
                ti.mode = e.mode
            elif e.kind == "file" and e.src is not None:
                ti.mode = stat.S_IMODE(e.src.stat().st_mode)
            else:
                ti.mode = 0o644
            ti.size = len(data)
            ti.pax_headers[DATA_HASH_KEY] = hashlib.sha1(data).hexdigest()
            tar.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def gzip_segment(data: bytes) -> bytes:
    buf = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buf, mtime=0) as gz:
        gz.write(data)
    return buf.getvalue()


def build_signature_tar(control_gz: bytes, key: Path,
                        key_name: str, epoch: int) -> bytes:
    """RSA/SHA-256 signature over the control gzip stream.

    With datahash present in .PKGINFO (mandatory since apk 2.14), apk
    verifies this signature at the control→data stream boundary, before any
    data bytes are digested — so it covers the control segment only.  The
    data segment is authenticated by the datahash itself."""
    proc = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(key)],
        input=control_gz, capture_output=True)
    if proc.returncode != 0:
        die(f"openssl signing failed: {proc.stderr.decode(errors='replace')}")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        ti = _tarinfo(f".SIGN.RSA256.{key_name}", epoch)
        ti.mode = 0o644
        ti.size = len(proc.stdout)
        tar.addfile(ti, io.BytesIO(proc.stdout))
    return strip_tar_trailer(buf.getvalue())


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Package A20OS build artifacts as an apk (v2) package.")
    ap.add_argument("recipe", type=Path, help="path to a recipe TOML file")
    ap.add_argument("--arch", required=True,
                    help="target apk architecture (riscv64, x86_64, ...)")
    ap.add_argument("-o", "--outdir", type=Path, default=None,
                    help="output directory (default: build/packages/<arch>)")
    ap.add_argument("--build-dir", type=Path, default=None,
                    help="userland build dir (default: user/build/<variant>)")
    ap.add_argument("--extra-dir", type=Path, default=None,
                    help="extra build dir (default: user/build/extra/<arch>)")
    ap.add_argument("--kernel-build-dir", type=Path, default=None,
                    help="kernel build dir (default: .kernel-build/<arch>)")
    ap.add_argument("--variant", default=None,
                    help="user build variant (default: <arch> or A20_VARIANT)")
    ap.add_argument("--repo-root", type=Path, default=None,
                    help="repository root (default: parent of tools/)")
    ap.add_argument("--sign-key", type=Path, default=None,
                    help="PEM RSA private key; adds a signature segment")
    ap.add_argument("--key-name", default=None,
                    help="key file name embedded in the signature entry "
                         "(default: <sign-key basename>.pub)")
    ap.add_argument("--set", dest="sets", action="append", default=[],
                    metavar="NAME=VALUE", help="extra placeholder value")
    ap.add_argument("--check", action="store_true",
                    help="validate the recipe without writing the package")
    args = ap.parse_args()

    repo_root = (args.repo_root or Path(__file__).resolve().parent.parent)
    arch = args.arch
    variant = args.variant or os.environ.get("A20_VARIANT") or arch

    recipe = load_recipe(recipe_path := args.recipe.resolve())
    pkg = recipe.package

    archs = pkg.get("archs")
    if archs and arch not in archs:
        die(f"{recipe_path.name}: package '{pkg['name']}' does not support "
            f"arch '{arch}' (supports: {', '.join(archs)})")

    ph = Placeholders(
        arch=arch,
        variant=variant,
        repo=str(repo_root),
        name=str(pkg["name"]),
        version=str(pkg["version"]),
        build_dir=str(args.build_dir or repo_root / "user" / "build" / variant),
        extra_dir=str(args.extra_dir or repo_root / "user" / "build" / "extra" / arch),
        kernel_build=str(args.kernel_build_dir or repo_root / ".kernel-build" / arch),
    )
    for key, value in recipe.vars.items():
        ph[key] = substitute(str(value), ph, f"{recipe_path.name}: [vars] {key}")
    for item in args.sets:
        if "=" not in item:
            die(f"--set expects NAME=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        ph[key] = value

    entries = resolve_files(recipe, ph)
    if not entries:
        die(f"{recipe_path.name}: recipe produced an empty package")

    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))

    # The data stream is built first: .PKGINFO embeds its SHA-256.
    data_gz = gzip_segment(build_data_tar(entries, epoch))
    datahash = hashlib.sha256(data_gz).hexdigest()
    pkginfo = pkginfo_text(pkg, arch, entries, epoch, datahash)

    if args.check:
        print(f"{pkg['name']}-{pkg['version']}-r{int(pkg.get('release', 0))}: "
              f"{len(entries)} entries OK")
        return

    control_gz = gzip_segment(build_control_tar(pkginfo, epoch))

    segments = b""
    if args.sign_key:
        key_name = args.key_name or args.sign_key.name + ".pub"
        segments += gzip_segment(
            build_signature_tar(control_gz, args.sign_key, key_name, epoch))
    segments += control_gz + data_gz

    outdir = args.outdir or repo_root / "build" / "packages" / arch
    outdir.mkdir(parents=True, exist_ok=True)
    out = outdir / f"{pkg['name']}-{pkg['version']}-r{int(pkg.get('release', 0))}.apk"

    tmp = out.with_suffix(out.suffix + ".tmp")
    tmp.write_bytes(segments)
    os.replace(tmp, out)
    print(f"mka20pkg: wrote {out} ({out.stat().st_size} bytes, "
          f"{len(entries)} entries)")


if __name__ == "__main__":
    main()
