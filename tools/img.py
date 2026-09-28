#!/usr/bin/env python3
"""FAT32/ext4 image assembly that used to live in tools/targets-images.mk.

Assembling a filesystem image is not compilation: it is dd, mkfs, and a long
list of mcopy calls whose order and error handling were expressed in shell.  All
of that is here now; the Makefile keeps the prerequisites (which *are* builds)
and passes in the values it owns.

The Makefile still owns every input -- image path and size, the user build
directory, which driver packages go where, the libc path, the protocols table.
This module never re-derives any of them, for the reason a20_make.query_make
documents: a second copy of a Makefile formula rots silently.
"""

from __future__ import annotations

import argparse
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def unescape(text: str) -> str:
    """Turn the literal \\n / \\t sequences make passed through into real ones.

    The make recipe carried them inside single quotes, so the shell handed the
    two characters backslash-n to printf, which turned them into newlines.
    Python gets the same two characters but printf is not involved, so the
    conversion has to happen here -- otherwise /etc/os-release lands in the
    image as one line of visible "\n".
    """
    text = text.replace("\\n", "\n").replace("\\t", "\t")
    # shlex undoes the shell quoting that produced the leading/trailing quotes
    # around each protocol entry, which plain .split() would leave in the file.
    return shlex.split(text)[0] if text.count("'") >= 2 else text.replace("\\\\", "\\")


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    print(f"+ {' '.join(cmd)}", flush=True)
    return subprocess.run(cmd, cwd=REPO, check=False, **kw)


def must(cmd: list[str]) -> None:
    r = run(cmd)
    if r.returncode != 0:
        raise SystemExit(r.returncode)


def mcopy(img: str, src: str, dest: str, *, overwrite: bool = True) -> None:
    cmd = ["mcopy"]
    if overwrite:
        cmd.append("-o")
    cmd += ["-i", img, src, dest]
    r = run(cmd)
    if r.returncode != 0:
        raise SystemExit(r.returncode)


def mdir(img: str, path: str) -> None:
    # The original used `-mmd ... || true`: these are directory creations that
    # legitimately fail when the directory already exists, so failure is not
    # an error.  Preserved deliberately.
    run(["mmd", "-i", img, path])


def build_fat32(a) -> int:
    img = REPO / a.fat32_img
    img.parent.mkdir(parents=True, exist_ok=True)
    must(["dd", f"if=/dev/zero", f"of={img}", "bs=1048576", f"count={a.fat32_mb}"])
    must([a.mkfs_fat, "-F", "32", str(img)])

    user_build = Path(a.user_build_dir)
    # The original iterated the shell glob `$(USER_BUILD_DIR)/*`, which does not
    # match dotfiles.  iterdir() would copy .build-id and friends into the
    # image, so filter them back out to keep the image byte-comparable.
    for f in sorted(user_build.iterdir()):
        if not f.is_file() or f.name.startswith("."):
            continue
        mcopy(str(img), str(f), f"::/{f.name}")

    # sh and bash are the mksh binary, as symlinks.
    for alias in ("sh", "bash"):
        mcopy(str(img), str(user_build / "mksh"), f"::/{alias}")

    for d in ("::/etc", "::/lib", "::/lib/drivers", "::/musl", "::/musl/lib"):
        mdir(str(img), d)

    for m in a.runtime_drvmod.split():
        mcopy(str(img), str(user_build / m), f"::/lib/drivers/{m}")
    for u in a.driver_store.split():
        mcopy(str(img), str(user_build / u), f"::/lib/drivers/{u}")

    # libc and libgcc are optional: absent on hosts that do not build them.
    if a.libc and (REPO / a.libc).is_file():
        mcopy(str(img), a.libc, "::/musl/lib/libc.so")
    if a.libgcc:
        p = Path(a.libgcc)
        if p.is_file():
            mcopy(str(img), a.libgcc, "::/lib/libgcc_s.so.1")

    # Content written from stdin in the original.
    blobs = (
        ("\n".join(shlex.split(a.protocols)) + "\n", "::/etc/protocols"),
        (unescape(a.os_release), "::/etc/os-release"),
        (unescape(a.test_txt), "::/test.txt"),
    )
    for text, dest in blobs:
        r = subprocess.run(["mcopy", "-o", "-i", str(img), "-", dest],
                           cwd=REPO, input=text, text=True, check=False)
        if r.returncode != 0:
            raise SystemExit(r.returncode)

    print(f"img: FAT32 image written: {img}")
    return 0


def build_ext4(a) -> int:
    img = REPO / a.ext4_img
    img.parent.mkdir(parents=True, exist_ok=True)
    must(["dd", f"if=/dev/zero", f"of={img}", "bs=1048576", f"count={a.ext4_mb}"])
    must(["mkfs.ext4", "-q", "-F", str(img)])
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command", choices=["fat32", "ext4"])
    for f in ("fat32-img", "fat32-mb", "ext4-img", "ext4-mb", "user-build-dir",
              "mkfs-fat", "runtime-drvmod", "driver-store", "libc", "libgcc",
              "protocols", "os-release", "test-txt"):
        ap.add_argument(f"--{f}", default="")
    a = ap.parse_args()
    return {"fat32": build_fat32, "ext4": build_ext4}[a.command](a)


if __name__ == "__main__":
    raise SystemExit(main())
