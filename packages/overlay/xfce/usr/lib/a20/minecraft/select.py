#!/usr/bin/env python3
"""Select Minecraft data by the native ELF architecture, not directory order."""
import argparse
import json
from pathlib import Path
import re
import sys

MACHINES = {"x86_64": 62, "aarch64": 183, "riscv64": 243}


def compatible(home, arch):
    if not (home / "client.jar").is_file():
        return False
    natives = home / "natives"
    if not (natives / "liblwjgl.so").is_file():
        return False
    for library in natives.glob("*.so"):
        try:
            with library.open("rb") as source:
                header = source.read(20)
        except OSError:
            return False
        if (len(header) != 20 or header[:6] != b"\x7fELF\x02\x01"
                or int.from_bytes(header[18:20], "little") != MACHINES.get(arch)):
            return False
    return True


def version(home):
    with (home / "version.json").open() as source:
        return json.load(source)["id"]


def candidates(media, arch):
    # Numeric ordering makes 1.10 newer than 1.9. Architecture suffixes do not
    # decide compatibility; custom directory names and legacy downloads work.
    def key(home):
        return [(1, int(s)) if s.isdigit() else (0, s)
                for s in re.split(r"(\d+)", home.name)]
    return sorted((p for p in media.glob("*") if compatible(p, arch)), key=key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--media", type=Path, default=Path("/usr/share/a20-media"))
    parser.add_argument("--arch", required=True)
    parser.add_argument("--home", default="")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--version", action="store_true")
    args = parser.parse_args()
    arch = {"amd64": "x86_64", "arm64": "aarch64"}.get(args.arch, args.arch)
    homes = [Path(args.home)] if args.home else candidates(args.media, arch)
    if args.all:
        print(" ".join(str(p) for p in homes if compatible(p, arch)))
        return 0
    if not homes or not compatible(homes[-1], arch):
        print(f"minecraft: no {arch} client and natives in {args.home or args.media}; "
              "fetch with tools/fetch-minecraft.sh VERSION OUTDIR ARCH", file=sys.stderr)
        return 1
    try:
        print(version(homes[-1]) if args.version else homes[-1])
    except (OSError, ValueError, KeyError) as error:
        print(f"minecraft: invalid version.json: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
