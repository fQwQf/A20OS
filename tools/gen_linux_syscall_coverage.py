#!/usr/bin/env python3

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
