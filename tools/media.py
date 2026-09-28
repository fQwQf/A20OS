#!/usr/bin/env python3
"""Write boot images to a physical board's media (was tools/target-console.mk).

`dd` to a block node is the one irreversible command in the build system, so the
guards are the substance of this file and the copy is an afterthought.  Each
guard is a separate function so a test can drive it without a real disk: the
mistyped-node case destroys the host's filesystem, and that mistake must be
reachable only in tests.

The board, media node and image list stay in the Makefile (TARGET_MEDIA_DEVICE,
TARGET_BOOT_MEDIA) and arrive as arguments.  Nothing here re-derives them.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def _fail(message: str) -> int:
    print(f"target-write-media: {message}", file=sys.stderr)
    return 1


def check_named(boot_media: str, media_device: str) -> int | None:
    if not boot_media:
        return _fail("TARGET_BOOT_MEDIA is empty")
    if not media_device:
        return _fail("TARGET_MEDIA_DEVICE is empty")
    return None


def check_block_device(media_device: str) -> int | None:
    try:
        is_block = Path(media_device).is_block_device()
    except OSError as exc:
        return _fail(f"{media_device}: {exc}")
    if not is_block:
        return _fail(f"{media_device} is not a block device;\n"
                     "  refusing to dd to it (a mistyped node destroys the "
                     "host's disk)")
    return None


def is_mounted(media_device: str) -> bool:
    if shutil.which("mountpoint") is None:
        return False
    r = subprocess.run(["mountpoint", "-q", media_device],
                       check=False, capture_output=True)
    return r.returncode == 0


def check_not_mounted(media_device: str) -> int | None:
    if is_mounted(media_device):
        return _fail(f"{media_device} is mounted; unmount it first")
    return None


def write(media_device: str, image: Path) -> int:
    print(f"  writing {image} -> {media_device}")
    r = subprocess.run(
        ["dd", f"if={image}", f"of={media_device}", "bs=4M", "conv=fsync",
         "status=progress"], check=False)
    if r.returncode != 0:
        return r.returncode
    subprocess.run(["sync"], check=False)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--boot-media", required=True,
                    help="space-separated image paths, in write order")
    ap.add_argument("--media-device", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    for guard in (lambda: check_named(a.boot_media, a.media_device),
                  lambda: check_block_device(a.media_device),
                  lambda: check_not_mounted(a.media_device)):
        rc = guard()
        if rc is not None:
            return rc

    images = [Path(p) for p in a.boot_media.split()]
    for image in images:
        if not image.is_file():
            return _fail(f"missing image {image}")

    if a.dry_run:
        for image in images:
            print(f"  would write {image} -> {a.media_device}")
        return 0

    for image in images:
        rc = write(a.media_device, image)
        if rc != 0:
            return rc
    print("target-write-media: done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
