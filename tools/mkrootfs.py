#!/usr/bin/env python3
"""mkrootfs — compose an A20OS root filesystem image from a world file.

A world file (packages/world/*.world) lists apk package names, one per line
(`#` comments allowed; `name=version` pins work).  mkrootfs resolves them
against the A20OS package repository (build/repo/<arch>, built by
`make pkg-repo`) plus the Alpine Linux repositories, installs everything
into a staging root with the real apk resolver, applies optional overlays,
and packs the result as an ext4 image (or a plain directory).

Examples:
    tools/mkrootfs.py --arch riscv64 --world packages/world/base.world
    tools/mkrootfs.py --arch riscv64 --world packages/world/devel.world \
        --output build/images/devel-riscv64.img --size-mb 1024
    tools/mkrootfs.py --arch riscv64 --world my.world --format dir \
        --usermode --allow-untrusted --output /tmp/rootfs

Privileges: like user/rootfs/alpine/build.sh, composing with correct file
ownership needs root; the script re-execs the apk/mkfs steps through sudo
when not root.  --usermode skips that for development (files stay owned by
the caller; ext4 output is forced to root ownership via -E root_owner).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_ALPINE_MIRROR = "https://mirrors.ustc.edu.cn/alpine"
DEFAULT_ALPINE_VERSION = "v3.23"
ALPINE_KEYS_REPO = "edge/main/x86_64"

# ext4 feature set matching tools/targets-images.mk (the kernel's ext4
# implementation is verified against exactly these features).
EXT4_MKFS_ARGS = [
    "mkfs.ext4", "-q", "-F",
    "-O", "^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index",
]


def die(msg: str) -> "SystemExit":
    print(f"mkrootfs: error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def run(cmd: list[str], sudo: list[str], **kw) -> None:
    proc = subprocess.run(sudo + cmd, **kw)
    if proc.returncode != 0:
        die(f"command failed ({proc.returncode}): {' '.join(sudo + cmd)}")


def read_world(paths: list[Path]) -> list[str]:
    pkgs: list[str] = []
    for path in paths:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            die(f"cannot read world file {path}: {exc}")
        for ln, line in enumerate(lines, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            if any(c.isspace() for c in line):
                die(f"{path}:{ln}: whitespace inside package spec {line!r}")
            pkgs.append(line)
    if not pkgs:
        die("world file(s) produced an empty package set")
    return pkgs


def ensure_alpine_keys(mirror: str, dest: Path) -> None:
    """Fetch Alpine's official repository signing keys (once, cached).

    The alpine-keys package ships /etc/apk/keys/*.rsa.pub; the same keys sign
    every architecture, so the host-arch package is fine.  Downloaded without
    trust (bootstrap problem); the fetched keys then authenticate everything
    else."""
    if any(dest.glob("*.rsa.pub")):
        return
    dest.mkdir(parents=True, exist_ok=True)
    import re
    import urllib.request
    mirrors = [mirror.rstrip("/"), "https://dl-cdn.alpinelinux.org/alpine"]
    name = None
    for m in mirrors:
        url = f"{m}/{ALPINE_KEYS_REPO}/"
        try:
            listing = urllib.request.urlopen(url, timeout=30).read().decode()
        except OSError:
            continue
        names = sorted(set(re.findall(r'alpine-keys-[^"<>]*?\.apk', listing)))
        if names:
            name = names[-1]
            break
    if not name:
        die("cannot resolve alpine-keys package from any mirror")
    data = None
    for m in mirrors:
        try:
            data = urllib.request.urlopen(f"{m}/{ALPINE_KEYS_REPO}/{name}",
                                          timeout=60).read()
            break
        except OSError:
            continue
    if data is None:
        die(f"cannot download alpine-keys package {name}")
    print(f"mkrootfs: fetched Alpine keys ({name})")
    import tarfile
    import io
    found = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        for member in tar.getmembers():
            if member.name.endswith(".rsa.pub") and member.isfile():
                target = dest / Path(member.name).name
                target.write_bytes(tar.extractfile(member).read())
                found += 1
    if not found:
        die(f"{names[-1]} contained no *.rsa.pub keys")


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Compose an A20OS rootfs image from a world file.")
    ap.add_argument("--arch", required=True,
                    help="apk architecture (riscv64, aarch64, x86_64, loongarch64)")
    ap.add_argument("--world", type=Path, action="append", required=True,
                    help="world file(s); repeatable")
    ap.add_argument("--repo", action="append", default=[],
                    help="A20OS package repository: a local directory "
                         "(its <arch>/ subdir is used) or a repository URL; "
                         "repeatable. Default: build/repo")
    ap.add_argument("--alpine-mirror", default=DEFAULT_ALPINE_MIRROR,
                    help="Alpine mirror tree base ('' disables Alpine repos)")
    ap.add_argument("--alpine-version", default=DEFAULT_ALPINE_VERSION)
    ap.add_argument("--no-alpine", action="store_true",
                    help="do not add Alpine repositories")
    ap.add_argument("--overlay", type=Path, action="append", default=[],
                    help="directory copied over the staged rootfs; repeatable")
    ap.add_argument("--output", type=Path, default=None,
                    help="output image (default: build/images/<world>-<arch>.img "
                         "or directory with --format dir)")
    ap.add_argument("--size-mb", type=int, default=512)
    ap.add_argument("--format", choices=["ext4", "dir"], default="ext4")
    ap.add_argument("--label", default="a20os-rootfs", help="ext4 volume label")
    ap.add_argument("--usermode", action="store_true",
                    help="no sudo; files stay owned by the caller")
    ap.add_argument("--keys-dir", type=Path, default=None,
                    help="apk trust directory (public keys)")
    ap.add_argument("--allow-untrusted", action="store_true",
                    help="accept unsigned packages/indexes")
    ap.add_argument("--cache-dir", type=Path, default=None,
                    help="apk package cache (default: build/cache/apk/<arch>)")
    ap.add_argument("--keep-staging", action="store_true")
    args = ap.parse_args()

    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent

    if args.size_mb < 16:
        die("--size-mb must be at least 16")

    # --- locate apk.static ---
    apk = os.environ.get("APK_STATIC")
    if not apk:
        proc = subprocess.run([str(script_dir / "ensure-apk-static.sh")],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            die(f"ensure-apk-static failed:\n{proc.stderr}")
        apk = proc.stdout.strip().splitlines()[-1]
    if not Path(apk).exists():
        die(f"apk binary not found: {apk}")

    pkgs = read_world(args.world)

    # --- repository list ---
    repos: list[str] = []          # passed to apk -X
    remote_repos: list[str] = []   # written to /etc/apk/repositories
    for repo in args.repo or [str(repo_root / "build" / "repo")]:
        if "://" in repo:
            repos.append(repo)
            remote_repos.append(repo)
        else:
            # apk appends /<arch>/ to every repository path itself; we only
            # verify the per-arch index exists before handing it over.
            local = Path(repo) / args.arch
            if not (local / "APKINDEX.tar.gz").exists():
                if args.repo:
                    die(f"local repository {local} has no APKINDEX.tar.gz; "
                        f"build it with `make pkg-repo ARCH={args.arch}` first")
                print(f"mkrootfs: note: default local repo {local} missing; "
                      f"using remote repositories only", file=sys.stderr)
                continue
            repos.append(str(Path(repo).resolve()))
    if not args.no_alpine and args.alpine_mirror:
        base = f"{args.alpine_mirror.rstrip('/')}/{args.alpine_version}"
        for section in ("main", "community"):
            repos.append(f"{base}/{section}")
            remote_repos.append(f"{base}/{section}")

    sudo: list[str] = []
    if os.geteuid() != 0 and not args.usermode:
        sudo = [os.environ.get("SUDO", "sudo")]

    keys_dir = args.keys_dir.resolve() if args.keys_dir else None
    if not args.allow_untrusted and not args.no_alpine and args.alpine_mirror:
        alpine_keys = repo_root / "build" / "cache" / "alpine-keys"
        ensure_alpine_keys(args.alpine_mirror, alpine_keys)
        combined = repo_root / "build" / "cache" / "apk-keys-combined"
        shutil.rmtree(combined, ignore_errors=True)
        combined.mkdir(parents=True)
        for src_dir in (alpine_keys, keys_dir):
            if src_dir is None or not Path(src_dir).is_dir():
                continue
            for pub in Path(src_dir).glob("*.rsa.pub"):
                shutil.copy2(pub, combined / pub.name)
        keys_dir = combined

    output = args.output
    if output is None:
        stem = "-".join(p.stem for p in args.world)
        suffix = "" if args.format == "dir" else ".img"
        output = repo_root / "build" / "images" / f"{stem}-{args.arch}{suffix}"

    staging = Path(tempfile.mkdtemp(prefix="a20os-rootfs-"))
    print(f"mkrootfs: staging at {staging}")
    try:
        cache_dir = args.cache_dir or repo_root / "build" / "cache" / "apk" / args.arch
        cache_dir.mkdir(parents=True, exist_ok=True)

        cmd = [
            apk, "--arch", args.arch,
            "--root", str(staging),
            "--initdb",
            "--cache-dir", str(cache_dir), "--cache-packages",
        ]
        if args.usermode:
            cmd.append("--usermode")
        if args.allow_untrusted:
            cmd += ["-U", "--allow-untrusted"]
        if keys_dir:
            cmd += ["--keys-dir", str(keys_dir)]
        for repo in repos:
            cmd += ["-X", repo]
        cmd += ["add"] + pkgs
        # apk --usermode applies the caller's umask to extracted files;
        # pin it so dev images get sane 0755/0644 permissions.
        preexec = (lambda: os.umask(0o022)) if args.usermode else None
        run(cmd, sudo, preexec_fn=preexec)

        for overlay in args.overlay:
            overlay = overlay.resolve()
            if not overlay.is_dir():
                die(f"overlay {overlay} is not a directory")
            run(["cp", "-a", "--remove-destination", f"{overlay}/.", f"{staging}/"], sudo)

        if remote_repos:
            apk_etc = staging / "etc" / "apk"
            run(["mkdir", "-p", str(apk_etc)], sudo)
            content = "".join(f"{r}\n" for r in remote_repos)
            run(["sh", "-c",
                 f"cat > {apk_etc / 'repositories'}"],
                sudo, input=content.encode())

        if args.format == "dir":
            output.parent.mkdir(parents=True, exist_ok=True)
            if output.exists():
                shutil.rmtree(output)
            if sudo:
                run(["chown", "-R", f"{os.getuid()}:{os.getgid()}",
                     str(staging)], [])
            shutil.move(str(staging), output)
            print(f"mkrootfs: rootfs directory at {output}")
            return

        output.parent.mkdir(parents=True, exist_ok=True)
        tmp_img = output.with_suffix(output.suffix + ".tmp")
        tmp_img.unlink(missing_ok=True)
        run(["truncate", "-s", f"{args.size_mb}M", str(tmp_img)], [])
        mkfs = EXT4_MKFS_ARGS + ["-L", args.label]
        mkfs_sudo = sudo
        if args.usermode:
            # Staging files are owned by the caller; record them as root in
            # the image.  fakeroot intercepts mkfs' ownership reads;
            # root_owner additionally fixes the inodes mkfs creates itself.
            if shutil.which("fakeroot"):
                mkfs = ["fakeroot"] + mkfs
            else:
                mkfs_sudo = []
                print("mkrootfs: warning: fakeroot not found; image files "
                      "will be owned by the caller", file=sys.stderr)
            mkfs += ["-E", "root_owner=0:0"]
        mkfs += ["-d", str(staging), str(tmp_img)]
        run(mkfs, mkfs_sudo)
        os.replace(tmp_img, output)
        print(f"mkrootfs: wrote {output} ({args.size_mb} MiB, "
              f"{len(pkgs)} packages)")
    finally:
        if staging.exists():
            if args.keep_staging:
                print(f"mkrootfs: staging kept at {staging}")
            elif sudo:
                subprocess.run(sudo + ["rm", "-rf", str(staging)])
            else:
                shutil.rmtree(staging, ignore_errors=True)


if __name__ == "__main__":
    main()
