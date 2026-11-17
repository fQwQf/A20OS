#!/usr/bin/env python3
"""Fetch checksum-pinned official M0Sense recovery assets; never flash them."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

COMMIT = "97156083b1ee3a94b7808bfaafd8a28ec78ed466"
BASE = f"https://raw.githubusercontent.com/sipeed/M0sense_BL702_example/{COMMIT}/"
ASSETS = (
    ("misc/firmware/m0sense_bootloader_v0.9.9.bin", "ce92ce6c7ef9c36fe4f95853dc9f2b68c691dc25633a8d9d911f5ce8bf53604b"),
    ("uf2_demos/blink_baremetal.uf2", "65a56b13cdd0775a11078a9ae5210cbd6afcbc4e5b6e8d40eb5eb4642ce3cb60"),
)


def fetch(out: Path, dry_run: bool = False) -> None:
    records = []
    for relative, digest in ASSETS:
        path = out / Path(relative).name
        url = BASE + relative
        records.append({"file": path.name, "sha256": digest, "source": url})
        if dry_run:
            print(f"DRY RUN: {url} -> {path} (sha256 {digest})")
            continue
        if path.exists():
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError(f"existing recovery asset has a different hash: {path}")
        else:
            with urllib.request.urlopen(url, timeout=30) as response:
                data = response.read(2 * 1024 * 1024 + 1)
            if len(data) > 2 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != digest:
                raise ValueError(f"official recovery asset checksum mismatch: {url}")
            out.mkdir(parents=True, exist_ok=True)
            with path.open("xb") as f:
                f.write(data)
        print(f"Verified official recovery asset: {path}")
    if dry_run:
        return
    (out / "sources.json").write_text(json.dumps({"sipeed_commit": COMMIT, "files": records}, indent=2) + "\n")
    (out / "README.md").write_text('''# Official M0Sense recovery assets

Downloaded byte-for-byte from the pinned Sipeed example repository; SHA256
verified. No flashing has been performed. Preserve these sources/hashes.

If the factory UF2 volume is missing, use the official DevCube MCU workflow:
short hardware BOOT strap pad to 3.3 V before power-on, select BL702 / MCU,
choose m0sense_bootloader_v0.9.9.bin, select the ROM serial device, set 2000000
baud, and use Create & Download. DevCube creates the board-specific bootheader.
Do not raw-dd this file or replace it with A20 probe-app.bin. Remove the BOOT
strap bridge and power-cycle, then hold software BOOT while tapping RESET.
A M0SENSE UF2 volume should appear; validate INFO_UF2.TXT before uploading.

This recovery replaces firmware and is a separate manual operation. First
check your board revision/pin labels and retain a backup of your own firmware.
The software BOOT button is GPIO2; the hardware BOOT strap pad is different.

blink_baremetal.uf2 is the official example to restore/test the application
using the existing M0SENSE UF2 volume. It is not a ROM recovery image.

Full steps: docs/zh/platforms/m0sense-bl702.md
Official guide: https://wiki.sipeed.com/hardware/en/maixzero/sense/start.html
''')


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=Path("build/m0sense-bl702/recovery"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    try:
        fetch(args.out, args.dry_run)
    except (OSError, ValueError) as error:
        ap.exit(1, f"M0Sense recovery preparation: {error}\n")


if __name__ == "__main__":
    main()
