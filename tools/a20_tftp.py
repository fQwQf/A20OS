#!/usr/bin/env python3
"""Serve exactly the manifest-verified kernel.bin from an A20 handoff bundle.

Read-only RFC1350, octet mode, mandatory 512-byte blocks. No uploads, directory
listing, arbitrary paths or serial access. Restart after packaging a new kernel.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import sys

from ls2k0300_handoff import _serve_bytes

MAX_KERNEL_BYTES = 64 * 1024 * 1024


def verified_kernel(bundle: Path) -> bytes:
    manifest = bundle / "manifest.json"
    kernel = bundle / "kernel.bin"
    for path in (manifest, kernel):
        if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
            raise ValueError(f"TFTP input must be a regular, non-symlink bundle file: {path}")
    if manifest.stat().st_size > 1024 * 1024:
        raise ValueError("bundle manifest is too large")
    metadata = json.loads(manifest.read_text())
    if not isinstance(metadata, dict) or not isinstance(metadata.get("files"), dict):
        raise ValueError("invalid A20 bundle file manifest")
    record = metadata["files"].get("kernel.bin")
    if (not isinstance(record, dict) or type(record.get("size")) is not int or
            not 0 < record["size"] <= MAX_KERNEL_BYTES or
            not isinstance(record.get("sha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
        raise ValueError("manifest must contain a valid bounded kernel.bin size and SHA256")
    if kernel.stat().st_size != record["size"]:
        raise ValueError("kernel.bin size differs from manifest; package this instance again")
    data = kernel.read_bytes()
    if len(data) != record["size"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise ValueError("kernel.bin hash differs from manifest; package this instance again")
    return data


def serve_tftp(bundle: Path, bind="0.0.0.0", port=69, *, once=False, on_ready=None) -> None:
    # The snapshot is checked before any listening socket opens; later changes
    # to files on disk cannot switch the bytes in a running transfer/server.
    data = verified_kernel(bundle)
    print(f"Verified kernel.bin SHA256 {hashlib.sha256(data).hexdigest()}", flush=True)
    _serve_bytes(data, "kernel.bin", bind, port, once=once, on_ready=on_ready)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--bind", default="0.0.0.0", help="host IPv4 address on the board's LAN")
    parser.add_argument("--port", type=int, default=69, help="TFTP listener port; U-Boot normally uses UDP 69")
    parser.add_argument("--once", action="store_true", help="exit after one transfer attempt")
    args = parser.parse_args()
    try:
        serve_tftp(args.bundle, args.bind, args.port, once=args.once)
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError, TypeError) as error:
        print(f"a20-tftp: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
