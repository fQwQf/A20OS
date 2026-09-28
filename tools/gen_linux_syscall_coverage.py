#!/usr/bin/env python3
"""Re-order the per-syscall coverage table in syscall_coverage.md.

This does NOT author the Notes column.  Every row must already exist in
syscall_coverage.md; the generator's job is to make the row order match
syscall_table.def and to fail loudly when the two disagree.  Notes are written
by hand, because they are the only place a per-syscall compatibility judgement
is recorded.

Two properties this script deliberately has:

* It fails rather than repairs.  A name in syscall_table.def with no row, or a
  row for a name that is no longer registered, is an error -- silently dropping
  either would let the document drift away from the table it documents.
* It rewrites only the text between the BEGIN/END markers, so hand-written
  prose outside the block is never touched.

Usage: gen_linux_syscall_coverage.py   (no arguments; paths are fixed)
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "kernel/abi/linux/syscall_table.def"
DOCUMENT = ROOT / "kernel/abi/linux/syscall_coverage.md"
BEGIN = "<!-- LINUX_SYSCALL_COVERAGE_BEGIN -->"
END = "<!-- LINUX_SYSCALL_COVERAGE_END -->"
HEADER = "\n".join(
    (
        "| Syscall | Area | Level | Smoke Gate | Notes |",
        "| --- | --- | --- | --- | --- |",
    )
)


def main():
    table = TABLE.read_text()
    document = DOCUMENT.read_text()
    names = re.findall(r"^LINUX_SYSCALL\(([^,]+)", table, re.MULTILINE)
    start = document.index(BEGIN)
    end = document.index(END, start) + len(END)
    coverage = document[start:end]
    rows = {
        match.group(1): match.group(0)
        for match in re.finditer(r"^\| `([^`]+)` \|.*$", coverage, re.MULTILINE)
    }

    missing = [name for name in names if name not in rows]
    extra = sorted(set(rows) - set(names))
    if missing or extra:
        details = []
        if missing:
            details.append("missing annotations: " + ", ".join(missing))
        if extra:
            details.append("stale annotations: " + ", ".join(extra))
        raise SystemExit("; ".join(details))

    generated = BEGIN + "\n" + HEADER + "\n"
    generated += "\n".join(rows[name] for name in names) + "\n" + END
    updated = document[:start] + generated + document[end:]
    if updated != document:
        DOCUMENT.write_text(updated)


if __name__ == "__main__":
    main()
