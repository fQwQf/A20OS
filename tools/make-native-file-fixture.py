#!/usr/bin/env python3
"""Create a real FAT32 image and a lossless sparse ROM sector fixture.

Used by firmware selftest only. Empty sectors remain zero; reads execute the
firmware's filesystem. This does not model an SD controller or physical media.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--mkfs", default="/usr/sbin/mkfs.fat")
    args = parser.parse_args()
    directory = args.out_dir
    directory.mkdir(parents=True, exist_ok=True)
    data = bytes((i * 37 + 11) & 255 for i in range(2048))
    payload = directory / "INPUT.DAT"
    payload.write_bytes(data)
    os.utime(payload, (1700000000, 1700000000))
    image = directory / "files.img"
    with image.open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    subprocess.run([args.mkfs, "--invariant", "-F", "32", str(image)], check=True)
    subprocess.run(["mmd", "-i", str(image), "::/BIN"], check=True)
    subprocess.run(["mcopy", "-i", str(image), str(payload), "::/BIN/INPUT.DAT"], check=True)
    records = []
    full_hash = hashlib.sha256()
    sparse_hash = hashlib.sha256()
    with image.open("rb") as stream:
        for lba in range(image.stat().st_size // 512):
            block = stream.read(512)
            full_hash.update(block)
            if any(block):
                records.append((lba, block))
            sparse_hash.update(block if any(block) else bytes(512))
    assert full_hash.digest() == sparse_hash.digest()
    header = ["/* Generated lossless FAT32 sector fixture; selftest only. */",
              f"#define FLAT_FIXTURE_SECTORS {image.stat().st_size // 512}U",
              f"#define FLAT_FIXTURE_RECORDS {len(records)}U",
              "static const uint32_t flat_fixture_lba[] = {" +
              ",".join(str(lba) for lba, _ in records) + "};",
              "static const uint8_t flat_fixture_data[][512] = {"]
    for _, block in records:
        header.append("{" + ",".join(str(value) for value in block) + "},")
    header.append("};\n")
    (directory / "flat-file-fixture.h").write_text("\n".join(header))
    (directory / "fixture.json").write_text(json.dumps({
        "image_sha256": full_hash.hexdigest(), "bytes": image.stat().st_size,
        "stored_sectors": len(records), "sector_bytes": 512,
        "payload_sha256": hashlib.sha256(data).hexdigest(),
        "storage": "lossless sparse ROM fixture; not SD hardware"}, indent=2) + "\n")


if __name__ == "__main__":
    main()
