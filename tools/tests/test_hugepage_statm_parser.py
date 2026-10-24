#!/usr/bin/env python3
"""Exercise hugepage_test's production /proc/self/statm parser."""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "user/cmds/stress/hugepage_test.c"

PRELUDE = r"""
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_BYTES 4096UL
#define STATM_PARSE_INVALID (-1)
#define STATM_PARSE_OVERFLOW (-2)
"""

TESTS = r"""
static int expect_valid(const char *line, unsigned long expected, const char *name)
{
    unsigned long rss_kb = 0;
    int rc = parse_statm_rss_kb(line, &rss_kb);
    if (rc != 0 || rss_kb != expected) {
        fprintf(stderr, "FAIL %s: rc=%d rss_kb=%lu input=[%s]\n",
                name, rc, rss_kb, line);
        return 0;
    }
    return 1;
}
static int expect_invalid(const char *line, int expected_rc, const char *name)
{
    unsigned long rss_kb = 0;
    int rc = parse_statm_rss_kb(line, &rss_kb);
    if (rc != expected_rc) {
        fprintf(stderr, "FAIL %s: rc=%d expected=%d input=[%s]\n",
                name, rc, expected_rc, line);
        return 0;
    }
    return 1;
}
int main(void)
{
    char overflow[128];
    snprintf(overflow, sizeof(overflow), "1 %lu 0 0 0 0 0\n",
             ULONG_MAX / 4UL + 1UL);
    if (!expect_valid("1 15 0 0 0 0 0\n", 60, "A20 proc sample 1") ||
        !expect_valid("3 16 0 0 0 0 0\n", 64, "A20 proc sample 2") ||
        !expect_valid("123456789012 2000000 123 4 5 6 7\n", 8000000,
                      "large Linux seven-column sample") ||
        !expect_valid("2 3 4 5 6 7 8 \t\n", 12,
                      "trailing whitespace") ||
        !expect_invalid("1 2 3 4 5 6\n", STATM_PARSE_INVALID,
                        "missing seventh column") ||
        !expect_invalid("1 2 3 4 5 6 7 junk\n", STATM_PARSE_INVALID,
                        "extra eighth token") ||
        !expect_invalid("1 nope 3 4 5 6 7\n", STATM_PARSE_INVALID,
                        "malformed numeric field") ||
        !expect_invalid(overflow, STATM_PARSE_OVERFLOW,
                        "RSS KiB multiplication overflow") ||
        !expect_invalid("1 18446744073709551616 0 0 0 0 0\n",
                        STATM_PARSE_OVERFLOW, "unsigned long token overflow"))
        return 1;
    puts("force-huge statm parser host checks passed");
    return 0;
}
"""


def extract_function(source: str) -> str:
    signature = "static int parse_statm_rss_kb("
    start = source.find(signature)
    if start < 0:
        raise SystemExit("parse_statm_rss_kb() not found")
    brace = source.find("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise SystemExit("unterminated parse_statm_rss_kb()")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    parser_function = extract_function(SOURCE.read_text())
    with tempfile.TemporaryDirectory(prefix="statm-parser-host-") as temp:
        cfile = pathlib.Path(temp) / "parser.c"
        binary = pathlib.Path(temp) / "parser"
        cfile.write_text(PRELUDE + parser_function + TESTS)
        subprocess.run([args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
    if result.returncode:
        print(result.stdout, end="")
        print(result.stderr, end="")
        raise SystemExit("statm parser host fixture failed")
    print(result.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
