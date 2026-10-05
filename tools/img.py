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
import struct
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


def mcopy_text(img: str, text: str, dest: str) -> None:
    """Write literal text into the image, as `printf ... | mcopy -` did."""
    r = subprocess.run(["mcopy", "-o", "-i", img, "-", dest],
                       cwd=REPO, input=text, text=True, check=False)
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

    # The kernel the hypervisor boots as a guest (smoke-hyp-a20os).  It is a
    # file on the image rather than a build product of the image because the
    # user program that loads it is itself on the image: /hyp_boot has no other
    # way to reach the ELF.  riscv64 only; empty elsewhere.
    if a.guest_kernel and (REPO / a.guest_kernel).is_file():
        mdir(str(img), "::/boot")
        mcopy(str(img), a.guest_kernel, "::/boot/guest-kernel.elf")

    print(f"img: FAT32 image written: {img}")
    return 0


# ---- JBD2 journal superblock surgery -------------------------------------
#
# mke2fs 1.47.2 leaves feature_incompat at 0 and s_checksum_type at 0 on a
# journal it creates: the log has no descriptor, commit or data checksums at
# all.  A kernel that mounts such a log cannot tell a complete transaction from
# one whose tail was torn by a power cut, which is the only thing the journal
# exists for, so ext4_journal.c declines those images outright.  e2fsprogs
# understands journal_checksum_v3 (dumpe2fs prints it) but never turns it on
# during creation, and there is no mke2fs or tune2fs knob that does either.
#
# So the flag is set here, after mkfs, on the finished image: the same three
# fields e2fsprogs would have written, plus the crc32c it would have computed.
# dumpe2fs and e2fsck both accept the result -- `e2fsck -fn` is clean on the
# patched image -- so this stays an image the rest of the ext4 world can read,
# and the guest is running against real csum_v3 semantics rather than against a
# private format that happens to satisfy our own parser.
#
# Layout (all offsets are from the start of the journal superblock block):
#   40  s_feature_incompat   64bit | csum_v3
#   80  s_checksum_type      JBD2_CRC32C_CHKSUM
#   252 s_checksum           crc32c seeded ~0 over the first 1024 bytes,
#                            with the checksum field itself read as zero
#
# Finding the block needs the ext4 superblock, the group 0 descriptor and the
# extent tree of the journal inode, so this is three small parses rather than a
# `debugfs -R "blocks <8>"` whose output format is nobody's contract.

JBD2_MAGIC = 0xC03B3998
JBD2_SUPERBLOCK_V2 = 4
JBD2_FEATURE_INCOMPAT_64BIT = 0x2
JBD2_FEATURE_INCOMPAT_CSUM_V3 = 0x10
JBD2_CRC32C_CHKSUM = 4
JBD2_SB_INCOMPAT_OFF = 40
JBD2_SB_CHECKSUM_TYPE_OFF = 80
JBD2_SB_CHECKSUM_OFF = 252
JBD2_SB_CHECKSUM_BYTES = 1024

EXT4_SB = 1024
EXT4_INCOMPAT_64BIT = 0x80
EXT4_S_INODE_SIZE = 0x58
EXT4_S_DESC_SIZE = 0xFE
EXT4_S_FIRST_DATA_BLOCK = 0x14
EXT4_S_JOURNAL_INUM = 0xE0
EXT4_INODE_IBLOCK = 40
EXTENT_MAGIC = 0xF30A


def _crc32c(seed: int, data: bytes) -> int:
    """The reflected Castagnoli polynomial e2fsprogs calls crc32c.

    Seeded with 0xffffffff and *not* final-xored, which is what every ext4 and
    JBD2 checksum on disk is: the code in kernel/fs/diskfs/ext4_journal.c
    computes the same value, and a mismatch between the two would show up
    immediately as an unreadable superblock.
    """
    table = _crc32c.table
    if table is None:
        table = []
        for i in range(256):
            crc = i
            for _ in range(8):
                crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
            table.append(crc)
        _crc32c.table = table
    crc = seed
    for byte in data:
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc & 0xFFFFFFFF


_crc32c.table = None


def _le32(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off:off + 4], "little")


def _le16(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off:off + 2], "little")


def _be32(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off:off + 4], "big")


def _journal_superblock_offset(img: bytes) -> int:
    """Byte offset of the JBD2 superblock, found by walking inode 8's extents."""
    def first_extent(off: int) -> int:
        # The journal of a 4 KiB-block filesystem is small enough to be a
        # single leaf, but the index walk is here so a larger image does not
        # silently misparse instead of failing.
        magic, entries, _max, depth = struct.unpack_from("<HHHH", img, off)
        if magic != EXTENT_MAGIC or not entries:
            raise SystemExit("ext4-journal: journal inode has no extent tree")
        if depth == 0:
            # extent: ee_block u32, ee_len u16, ee_start_hi u16, ee_start_lo u32
            return _le32(img, off + 20) | (_le16(img, off + 18) << 32)
        leaf = _le32(img, off + 16) | (_le16(img, off + 20) << 32)
        return first_extent(leaf * block_size)

    sb = img[EXT4_SB:EXT4_SB + 1024]
    if _le16(sb, 0x38) != 0xEF53:
        raise SystemExit("ext4-journal: not an ext4 image")
    block_size = 1024 << _le32(sb, 0x18)
    incompat = _le32(sb, 0x60)
    if incompat & EXT4_INCOMPAT_64BIT:
        inode_size = _le16(sb, EXT4_S_INODE_SIZE)
        desc_size = _le16(sb, EXT4_S_DESC_SIZE)
    else:
        inode_size, desc_size = 128, 32
    # The descriptor table follows the primary superblock, whatever the
    # feature flags say: block 1 for a 4 KiB filesystem (the superblock starts
    # at byte 0), block 2 for a 1 KiB one (the superblock starts at byte 1024,
    # so it shares block 1 with the boot sector's neighbour).
    gdt_block = _le32(sb, EXT4_S_FIRST_DATA_BLOCK) + 1
    group_desc = img[gdt_block * block_size:][:desc_size]
    inode_table = _le32(group_desc, 8)
    if desc_size >= 0x2C:
        inode_table |= _le32(group_desc, 0x28) << 32
    journal_inum = _le32(sb, EXT4_S_JOURNAL_INUM)
    if not journal_inum:
        raise SystemExit("ext4-journal: image has no journal inode")
    inode = inode_table * block_size + (journal_inum - 1) * inode_size
    return first_extent(inode + EXT4_INODE_IBLOCK) * block_size


def enable_journal_checksums(path: Path) -> None:
    data = bytearray(path.read_bytes())
    offset = _journal_superblock_offset(bytes(data))
    meta = bytearray(data[offset:offset + 4096])
    if _be32(meta, 0) != JBD2_MAGIC or _be32(meta, 4) != JBD2_SUPERBLOCK_V2:
        raise SystemExit("ext4-journal: inode 8 is not a JBD2 superblock")
    incompat = (_be32(meta, JBD2_SB_INCOMPAT_OFF) |
                JBD2_FEATURE_INCOMPAT_64BIT | JBD2_FEATURE_INCOMPAT_CSUM_V3)
    struct.pack_into(">I", meta, JBD2_SB_INCOMPAT_OFF, incompat)
    meta[JBD2_SB_CHECKSUM_TYPE_OFF] = JBD2_CRC32C_CHKSUM
    struct.pack_into(">I", meta, JBD2_SB_CHECKSUM_OFF, 0)
    checksum = _crc32c(0xFFFFFFFF, bytes(meta[:JBD2_SB_CHECKSUM_BYTES]))
    struct.pack_into(">I", meta, JBD2_SB_CHECKSUM_OFF, checksum)
    data[offset:offset + len(meta)] = meta
    path.write_bytes(bytes(data))
    print(f"img: journal superblock at block {offset // 4096}: "
          f"64bit|csum_v3, crc32c {checksum:#010x}")


def build_ext4(a) -> int:
    return _build_ext4(a, journal=bool(getattr(a, "ext4_journal", "")))


def build_ext4_journal(a) -> int:
    """An ext4 image *with* an internal JBD2 journal.

    The published dev images deliberately carry none (`^has_journal`): they
    are rebuilt from staging on every build, and a journal costs 4 MiB of
    ext4.img's 128 MiB.  The crash-consistency gate needs the opposite, so it
    gets its own image through the same staging path.
    """
    return _build_ext4(a, journal=True)


def _build_ext4(a, *, journal: bool) -> int:
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
        features = "extent,huge_file,flex_bg,uninit_bg,dir_index"
        if not journal:
            features = "^has_journal," + features
        cmd = [a.mkfs_ext4, "-F", "-O", features]
        if journal:
            # 4 KiB blocks, not the 1 KiB the journal-less dev images use: a
            # JBD2 log block is one filesystem block, and the reader/writer
            # (and the block cache page) all agree on 4 KiB.  Anything else
            # would mean copying a whole page into every log block.
            cmd += ["-b", "4096"]
        must(cmd + ["-d", str(staging), str(tmp)])
        if journal:
            enable_journal_checksums(tmp)
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


def build_vf2_minimal(a) -> int:
    """Clone the VF2 FAT32 image and drop the freshly built fastfetch into it.

    The recipe copied fat32.img to a variant and then used mtools to place one
    binary at the image root; both steps are image mutation, so they live here
    next to the other image builders.
    """
    src = REPO / a.src
    dst = REPO / a.dst
    shutil.copy2(src, dst)
    ff = REPO / a.payload
    mcopy(str(dst), str(ff), "::/fastfetch")
    print(f"img: {dst} + fastfetch at ::/fastfetch")
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


def build_release_disk(a) -> int:
    """The release disk.img: a bare FAT32 root, no /lib/drivers or /musl tree.

    Deliberately simpler than the dev FAT32 image -- this is the published
    artifact, and the recipe never filtered *.o/*.so here, only the shell glob
    hid dotfiles.  Both facts are load-bearing, so neither is "cleaned up".
    """
    img = REPO / a.disk_out
    img.parent.mkdir(parents=True, exist_ok=True)
    img.unlink(missing_ok=True)
    must([a.mkfs_fat, "-C", "-F", "32", str(img), "131072"])

    user_build = Path(a.user_build_dir)
    for f in sorted(user_build.iterdir()):
        if not f.is_file() or f.name.startswith("."):
            continue
        mcopy(str(img), str(f), f"::/{f.name}", overwrite=False)
    for alias in ("sh", "bash"):
        mcopy(str(img), str(user_build / "mksh"), f"::/{alias}")

    mdir(str(img), "::/etc")
    mdir(str(img), "::/lib")
    if a.libgcc:
        p = Path(a.libgcc)
        if p.is_file():
            mcopy(str(img), str(p), "::/lib/libgcc_s.so.1")
    for text, dest in (("\n".join(shlex.split(a.protocols)) + "\n",
                        "::/etc/protocols"),
                       ("external\n", "::/etc/external-root")):
        r = subprocess.run(["mcopy", "-o", "-i", str(img), "-", dest],
                           cwd=REPO, input=text, text=True, check=False)
        if r.returncode != 0:
            raise SystemExit(r.returncode)
    print(f"img: release disk written: {img}")
    return 0


EXT4_FEATURES = "^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index"


def build_scratch(a) -> int:
    """The fixed fixture disks the native/service smokes attach as extra disks.

    Not compilation: dd, mkfs and mcopy.  They live here with the other image
    builders so the two mkfs feature sets cannot drift apart.
    """
    img = REPO / a.scratch_out
    img.parent.mkdir(parents=True, exist_ok=True)
    mb = a.scratch_mb

    if a.scratch_kind == "zeros":
        img.unlink(missing_ok=True)
        must(["dd", "if=/dev/zero", f"of={img}", "bs=1M", f"count={mb}"])
    elif a.scratch_kind in ("fat-marker", "fat-hello"):
        img.unlink(missing_ok=True)
        must(["dd", "if=/dev/zero", f"of={img}", "bs=1M", f"count={mb}"])
        must([a.mkfs_fat, "-F", "32", str(img)])
        if a.scratch_kind == "fat-marker":
            # The marker lives at a fixed byte offset that the ubd driver
            # computes, so it is written raw rather than through a filesystem.
            r = subprocess.run(
                ["dd", f"of={img}", "bs=1", "seek=4096", "conv=notrunc"],
                input="A20OS-UBD-MARKER", text=True, check=False,
                capture_output=True)
            if r.returncode != 0:
                raise SystemExit(r.returncode)
            mcopy(str(img), a.payload, "::/big.bin")
        else:
            mcopy_text(str(img), "hello-uxfs\n", "::/hello.txt")
    elif a.scratch_kind == "ext4":
        staging = img.parent / "ufs-ext4-staging"
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir(parents=True)
        try:
            (staging / "hello.txt").write_text("hello ext4 user-space\n")
            img.unlink(missing_ok=True)
            must(["dd", "if=/dev/zero", f"of={img}", "bs=1M", f"count={mb}"])
            must(["mke2fs", "-q", "-F", "-O", EXT4_FEATURES,
                  "-d", str(staging), str(img)])
        finally:
            shutil.rmtree(staging, ignore_errors=True)
    elif a.scratch_kind == "ntfs":
        img.unlink(missing_ok=True)
        must(["dd", "if=/dev/zero", f"of={img}", "bs=1M", f"count={mb}"])
        r = subprocess.run(["mkfs.ntfs", "-F", "-Q", "-L", "A20NTFS", str(img)],
                           check=False, capture_output=True)
        if r.returncode != 0:
            raise SystemExit(r.returncode)
    else:
        raise SystemExit(f"error: unknown scratch kind {a.scratch_kind!r}")

    print(f"img: scratch {a.scratch_kind} written: {img}")
    return 0


def inject_sbase(a) -> int:
    """Copy the mlibc-on-sbase tools into the existing FAT32 rootfs.

    Unlike the other builders this does not create an image: it injects into the
    dev FAT32 image in place, which is why it has to be re-runnable (every mcopy
    is -o) and why the smoke that consumes it can point at the same file.
    """
    img = REPO / a.fat32_img
    native = REPO / a.native_build_dir
    for t in a.tools.split():
        mcopy(str(img), str(native / f"mlibc-{t}"), f"::/mlibc-{t}")
    # The source is test_mlibc_sbase.sh but the image gets a hyphenated name.
    mcopy(str(img), "user/tests/test_mlibc_sbase.sh", "::/test-mlibc-sbase.sh")
    print(f"img: sbase tools injected into {img}")
    return 0


def inject_mlibc(a) -> int:
    """Copy the mlibc test/mksh binaries into the existing FAT32 rootfs.

    Same in-place contract as inject_sbase: the dev image is composed before
    mlibc is built (meson is far too heavy to hang off every dev-build), so the
    mlibc smoke cases inject after the build instead.
    """
    img = REPO / a.fat32_img
    native = REPO / a.native_build_dir
    for name in ("mlibc-hello", "mlibc-child", "mlibc-fork",
                 "mlibc-sigchld", "mlibc-pipeexec", "mlibc-mksh"):
        mcopy(str(img), str(native / f"{name}-{a.tag}"), f"::/bin/{name}-{a.tag}")
    mcopy(str(img), "user/tests/test_mlibc_mksh.sh", "::/bin/test-mlibc-mksh.sh")
    print(f"img: mlibc binaries injected into {img}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command",
                    choices=["fat32", "ext4", "ext4-journal", "release-disk",
                             "scratch",
                             "sbase-rootfs", "mlibc-rootfs", "copy", "verify-vbox",
                             "vf2-minimal"])
    for f in (
              "fat32-img", "fat32-mb", "ext4-img", "ext4-mb", "ext4-staging-dir", "mkfs-ext4", "user-build-dir", "mkfs-fat", "runtime-drvmod", "driver-store", "libc", "libgcc", "protocols", "os-release", "test-txt", "guest-kernel", "src", "dst", "arch", "board", "abi", "bringup", "nommu", "opt", "stamp", "disk-out", "scratch-out", "scratch-mb", "scratch-kind", "payload", "native-build-dir", "tools", "tag" ):
        ap.add_argument(f"--{f}", default="")
    a = ap.parse_args()
    return {"fat32": build_fat32, "ext4": build_ext4,
            "ext4-journal": build_ext4_journal,
            "release-disk": build_release_disk, "scratch": build_scratch,
            "sbase-rootfs": inject_sbase,
            "mlibc-rootfs": inject_mlibc,
            "copy": copy_image,
            "verify-vbox": verify_vbox_rootfs,
            "vf2-minimal": build_vf2_minimal}[a.command](a)


if __name__ == "__main__":
    raise SystemExit(main())
