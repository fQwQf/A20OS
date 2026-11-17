#!/usr/bin/env python3
"""Stage BusyBox applets for the flat FAT layout without duplicating each ELF."""
import argparse
from pathlib import Path
import re
from prepare_busybox import write_changed


def stage(binary: Path, output: Path, applets: list[str]) -> None:
    output.mkdir(parents=True, exist_ok=True)
    links = binary.with_name(".busybox.links")
    if links.exists():
        supported = {Path(line).name for line in links.read_text().splitlines()}
        missing = set(applets) - supported
        if missing:
            raise ValueError(f"BusyBox configuration does not provide: {sorted(missing)}")
    # Retire artifacts left by older builds before image packagers glob this
    # directory. Other applet names are overwritten below.
    for name in ("mksh", "bash", "sponge", "xinstall", "join", "pathchk",
                 "busybox.links", "busybox.config", "busybox.source.json"):
        (output / name).unlink(missing_ok=True)
    # FAT cannot represent symlinks. Only sh needs an extra ELF; the small
    # scripts use the conventional /bin/sh and locate their sibling busybox.
    write_changed(output / "sh", binary.read_bytes())
    for name in applets:
        if name in ("busybox", "sh", "ash", "init"):
            continue
        if not re.fullmatch(r"[A-Za-z0-9_.+\[\]-]+", name):
            raise ValueError(f"invalid BusyBox applet {name!r}")
        write_changed(output / name, (
            '#!/bin/sh\n'
            'case "$0" in\n'
            f'  */*) exec "${{0%/*}}/busybox" {name} "$@" ;;\n'
            f'  *) exec busybox {name} "$@" ;;\n'
            'esac\n').encode())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--applets", required=True)
    args = parser.parse_args()
    stage(args.binary, args.output, args.applets.split())
