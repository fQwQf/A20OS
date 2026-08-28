#!/usr/bin/env python3
"""apk-sign-stream — prepend an RSA/SHA-256 signature stream to a gzip-segmented
apk artifact (APKINDEX.tar.gz or similar).

    apk-sign-stream.py --key KEY.rsa [--key-name NAME] FILE

Rewrites FILE in place as: [sig gzip stream][original FILE bytes], where the
sig stream is a tar (without end-of-archive marker, see mka20pkg.py) holding
.SIGN.RSA256.<key-name> — an RSA/SHA-256 signature over the original FILE
bytes.  This mirrors `abuild-sign` for indexes.
"""

from __future__ import annotations

import argparse
import gzip
import io
import subprocess
import sys
import tarfile
from pathlib import Path


def die(msg: str) -> "SystemExit":
    print(f"apk-sign-stream: error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--key", type=Path, required=True, help="PEM RSA private key")
    ap.add_argument("--key-name", default=None,
                    help="public key file name (default: <key basename>.pub)")
    ap.add_argument("file", type=Path)
    args = ap.parse_args()

    data = args.file.read_bytes()
    key_name = args.key_name or args.key.name + ".pub"

    proc = subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(args.key)],
                          input=data, capture_output=True)
    if proc.returncode != 0:
        die(f"openssl signing failed: {proc.stderr.decode(errors='replace')}")

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        ti = tarfile.TarInfo(f".SIGN.RSA256.{key_name}")
        ti.mtime = 0
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = "root"
        ti.mode = 0o644
        ti.size = len(proc.stdout)
        tar.addfile(ti, io.BytesIO(proc.stdout))
    tar_bytes = buf.getvalue()
    end = len(tar_bytes)
    while end >= 512 and tar_bytes[end - 512:end] == b"\0" * 512:
        end -= 512
    tar_bytes = tar_bytes[:end]

    out = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=out, mtime=0) as gz:
        gz.write(tar_bytes)

    tmp = args.file.with_suffix(args.file.suffix + ".signed.tmp")
    tmp.write_bytes(out.getvalue() + data)
    tmp.replace(args.file)
    print(f"apk-sign-stream: signed {args.file} with key name {key_name}")


if __name__ == "__main__":
    main()
