#!/usr/bin/env python3
"""Generate the built-in rootfs overlay C table from a directory tree."""

from __future__ import annotations

import argparse
import pathlib
import re


def c_ident(path: str) -> str:
    ident = re.sub(r"[^0-9A-Za-z_]", "_", path.strip("/"))
    return "rootfs_overlay_" + ident.strip("_")


def format_bytes(data: bytes) -> list[str]:
    if not data:
        return []

    values = [f"0x{byte:02x}" for byte in data]
    lines: list[str] = []
    for idx in range(0, len(values), 12):
        chunk = ", ".join(values[idx : idx + 12])
        lines.append(f"    {chunk}")
    return lines


def write_header(out_h: pathlib.Path) -> None:
    out_h.write_text(
        """#ifndef FS_ROOTFS_OVERLAY_H
#define FS_ROOTFS_OVERLAY_H

#include "core/types.h"

typedef struct {
    const char *path;
    const unsigned char *content;
    size_t size;
    uint32_t mode;
} rootfs_overlay_entry_t;

extern const rootfs_overlay_entry_t g_rootfs_overlay[];
extern const size_t g_rootfs_overlay_count;

#endif
""",
        encoding="utf-8",
    )


def collect_entries(root: pathlib.Path) -> list[tuple[str, pathlib.Path, bytes]]:
    entries: list[tuple[str, pathlib.Path, bytes]] = []
    for item in sorted(root.rglob("*")):
        if not item.is_file():
            continue
        rel = item.relative_to(root).as_posix()
        entries.append(("/" + rel, item, item.read_bytes()))
    return entries


def write_source(out_c: pathlib.Path, root: pathlib.Path) -> None:
    entries = collect_entries(root)

    lines = ['#include "fs/rootfs_overlay.h"', ""]
    for path, _item, data in entries:
        ident = c_ident(path)
        lines.append(f"static const unsigned char {ident}[] = {{")
        byte_lines = format_bytes(data)
        for idx, line in enumerate(byte_lines):
            suffix = "," if idx < len(byte_lines) - 1 else ""
            lines.append(line + suffix)
        lines.append("};")
        lines.append("")

    lines.append("const rootfs_overlay_entry_t g_rootfs_overlay[] = {")
    for path, item, data in entries:
        mode = item.stat().st_mode & 0o777
        lines.append(f'    {{ "{path}", {c_ident(path)}, {len(data)}, 0{mode:o} }},')
    lines.append("};")
    lines.append("")
    lines.append(
        "const size_t g_rootfs_overlay_count = "
        "sizeof(g_rootfs_overlay) / sizeof(g_rootfs_overlay[0]);"
    )
    lines.append("")

    out_c.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-c", required=True, type=pathlib.Path)
    parser.add_argument("--out-h", required=True, type=pathlib.Path)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    args = parser.parse_args()

    args.out_c.parent.mkdir(parents=True, exist_ok=True)
    args.out_h.parent.mkdir(parents=True, exist_ok=True)
    write_source(args.out_c, args.root)
    write_header(args.out_h)


if __name__ == "__main__":
    main()
