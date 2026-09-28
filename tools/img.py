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
import fcntl
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
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
    staging_parent = REPO / a.ext4_staging_dir
    staging_parent.parent.mkdir(parents=True, exist_ok=True)

    # Recursive gates can overlap builds that share BUILD_DIR.  The original
    # held an flock across staging, mkfs and the publish, and published with a
    # rename so a reader never sees a half-written image.
    lock = open(str(img) + ".lock", "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    staging = Path(tempfile.mkdtemp(dir=staging_parent.parent,
                                     prefix=staging_parent.name + "."))
    fd, tmp_name = tempfile.mkstemp(dir=img.parent, prefix=img.name + ".tmp.")
    os.close(fd)
    tmp = Path(tmp_name)
    try:
        user_build = Path(a.user_build_dir)
        # Same shell-glob semantics as the FAT32 rule: no dotfiles, files only.
        for f in sorted(user_build.iterdir()):
            if f.is_file() and not f.name.startswith("."):
                shutil.copy2(f, staging / f.name)
        for alias in ("sh", "bash"):
            shutil.copy2(user_build / "mksh", staging / alias)

        (staging / "test.txt").write_text(
            "Hello from ext4!\nThis file is on the ext4 filesystem.\n")
        etc = staging / "etc"
        etc.mkdir()
        (etc / "protocols").write_text("\n".join(shlex.split(a.protocols)) + "\n")
        (etc / "os-release").write_text(unescape(a.os_release))

        must(["dd", "if=/dev/zero", f"of={tmp}", "bs=1048576", f"count={a.ext4_mb}"])
        # The `^` binds to has_journal only; the rest are enables.  Passed
        # through verbatim because the on-disk feature set is what the guest
        # boots against.
        must([a.mkfs_ext4, "-F",
              "-O", "^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index",
              "-d", str(staging), str(tmp)])
        os.replace(tmp, img)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
        tmp.unlink(missing_ok=True)
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()

    print(f"img: ext4 image written: {img}")
    return 0


def copy_image(a) -> int:
    src, dst = REPO / a.src, REPO / a.dst
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    print(f"img: copied {src} -> {dst}")
    return 0


def verify_vbox_rootfs(a) -> int:
    """Rebuild the FAT32 image if its /init is not the freshly built one.

    The user build stamp is refreshed by a recipe, so a binary can become newer
    than an already-created FAT image within one checkout and make's timestamp
    graph would leave a bootable but stale userspace behind.  Comparing /init
    byte-for-byte catches that.
    """
    img = REPO / a.fat32_img
    expect = Path(a.user_build_dir) / "init"
    probe = REPO / ".kernel-build" / ".vbox-init-probe"
    probe.parent.mkdir(parents=True, exist_ok=True)

    def staged_init_matches() -> bool:
        probe.unlink(missing_ok=True)
        if run(["mcopy", "-i", str(img), "::/init", str(probe)]).returncode != 0:
            return False
        return probe.read_bytes() == expect.read_bytes()

    if not staged_init_matches():
        print("[VBOX] stale /init detected; rebuilding root filesystem")
        img.unlink(missing_ok=True)
        b = subprocess.run(
            ["make", f"ARCH={a.arch}", f"BOARD={a.board}", f"ABI={a.abi}",
             f"BRINGUP={a.bringup}", f"NOMMU={a.nommu}", f"OPT={a.opt}",
             a.fat32_img], cwd=REPO, check=False)
        if b.returncode != 0:
            return b.returncode
        if not staged_init_matches():
            print("[VBOX] /init still stale after rebuild", file=sys.stderr)
            return 1
    probe.unlink(missing_ok=True)

    stamp = REPO / a.stamp
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.touch()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command", choices=["fat32", "ext4", "copy", "verify-vbox"])
    for f in ("fat32-img", "fat32-mb", "ext4-img", "ext4-mb", "ext4-staging-dir",
              "mkfs-ext4", "user-build-dir", "mkfs-fat", "runtime-drvmod",
              "driver-store", "libc", "libgcc", "protocols", "os-release",
              "test-txt", "src", "dst", "arch", "board", "abi", "bringup",
              "nommu", "opt", "stamp"):
        ap.add_argument(f"--{f}", default="")
    a = ap.parse_args()
    return {"fat32": build_fat32, "ext4": build_ext4, "copy": copy_image,
            "verify-vbox": verify_vbox_rootfs}[a.command](a)


if __name__ == "__main__":
    raise SystemExit(main())
