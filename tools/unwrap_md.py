#!/usr/bin/env python3
"""Unwrap hard line-wrapped paragraphs in Markdown files.

Joins prose lines so sentences do not break mid-way, while preserving:
- code fences (``` / ~~~ blocks) byte-for-byte
- Markdown table rows (lines whose first non-space char is '|')
- ATX headings (#...)
- list item boundaries (a new `-`/`*`/`+`/`N.` at the same or shallower indent)
- blockquote (`>`) continuation

A paragraph = consecutive body lines (no special prefix) of the same kind.
List items keep their marker + indentation and absorb their deeper-indented
continuation lines.
"""

import re
import sys

FENCE_RE = re.compile(r"^(\s*)(```|~~~)")
HEADING_RE = re.compile(r"^#{1,6}\s")
TABLE_RE = re.compile(r"^\s*\|")
LIST_RE = re.compile(r"^(\s*)([-*+]|\d+[.)])(\s+)")
QUOTE_RE = re.compile(r"^(\s*)(>)(\s?)(.*)$")


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def main():
    path = sys.argv[1]
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")

    out = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]

        # Code fence: copy through the closing fence untouched.
        m = FENCE_RE.match(line)
        if m:
            out.append(line)
            i += 1
            while i < n and not FENCE_RE.match(lines[i]):
                out.append(lines[i])
                i += 1
            if i < n:
                out.append(lines[i])  # closing fence
                i += 1
            continue

        # Keep headings and table rows as single lines.
        if HEADING_RE.match(line) or TABLE_RE.match(line) or line.strip() == "":
            out.append(line)
            i += 1
            continue

        # Blockquote: join consecutive quote lines into one `>` line.
        q = QUOTE_RE.match(line)
        if q:
            indent = q.group(1)
            parts = [q.group(4).rstrip()]
            i += 1
            while i < n:
                nq = QUOTE_RE.match(lines[i])
                if nq and nq.group(1) == indent and not FENCE_RE.match(lines[i]):
                    parts.append(nq.group(4).strip())
                    i += 1
                else:
                    break
            text = " ".join(p for p in parts if p)
            out.append(indent + "> " + text.rstrip())
            continue

        # List item: keep marker/indent, absorb plain continuation prose lines.
        # A line that starts a list marker at ANY indentation (a nested or
        # sibling item) ends the current item instead of being merged in.
        lm = LIST_RE.match(line)
        if lm:
            marker = lm.group(1) + lm.group(2) + lm.group(3)
            body = [line[len(marker):].strip()]
            i += 1
            while i < n:
                cur = lines[i]
                if FENCE_RE.match(cur):
                    break
                if cur.strip() == "":
                    break
                if TABLE_RE.match(cur):
                    break
                if HEADING_RE.match(cur):
                    break
                if LIST_RE.match(cur):
                    break
                if QUOTE_RE.match(cur):
                    break
                body.append(cur.strip())
                i += 1
            out.append(marker + " ".join(p for p in body if p).rstrip())
            continue

        # Plain paragraph: join consecutive body lines.
        para = [line.strip()]
        i += 1
        while i < n:
            cur = lines[i]
            if FENCE_RE.match(cur):
                break
            if cur.strip() == "":
                break
            if TABLE_RE.match(cur):
                break
            if HEADING_RE.match(cur):
                break
            if LIST_RE.match(cur):
                break
            if QUOTE_RE.match(cur):
                break
            para.append(cur.strip())
            i += 1
        out.append(" ".join(para).rstrip())

    while out and out[-1] == "":
        out.pop()
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
