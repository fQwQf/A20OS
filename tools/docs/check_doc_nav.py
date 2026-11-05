#!/usr/bin/env python3
"""Check MkDocs navigation completeness, link validity, and anchor resolution.

Ensures:
  1. All files listed in site/mkdocs.yml nav exist under docs/.
  2. 100% of active markdown docs (excluding archive/ and research/) are indexed.
  3. All relative markdown links in active docs resolve to real files.
  4. All markdown anchor links (#anchor) match real headings in target docs.

Usage:
  python3 tools/docs/check_doc_nav.py
  python3 tools/docs/check_doc_nav.py --verbose
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOCS_DIR = os.path.join(ROOT, "docs")
MKDOCS_FILE = os.path.join(ROOT, "site/mkdocs.yml")

EXCLUDE_DIRS = {"archive", "research"}
HEADING_PATTERN = re.compile(r"^(#{1,6})\s+(.+)$", re.M)
LINK_PATTERN = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")


def slugify(text: str) -> str:
    """GitHub / MkDocs slugification for markdown headers."""
    text = text.strip().lower()
    text = re.sub(r"[^\w\s\u4e00-\u9fff-]", "", text)
    text = re.sub(r"[\s]+", "-", text)
    return text


def parse_mkdocs_nav(mkdocs_path: str) -> set:
    """Extract all markdown file paths referenced in mkdocs.yml nav."""
    nav_files = set()
    if not os.path.exists(mkdocs_path):
        raise FileNotFoundError(f"Missing {mkdocs_path}")
    with open(mkdocs_path, "r", encoding="utf-8") as f:
        in_nav = False
        for line in f:
            if line.strip().startswith("nav:"):
                in_nav = True
                continue
            if in_nav:
                # Capture all foo/bar.md occurrences in nav
                matches = re.findall(r"[\s:\'\"]+([\w\-./]+\.md)", line)
                for m in matches:
                    nav_files.add(m)
    return nav_files


def collect_active_docs(docs_dir: str) -> list:
    """Collect all active markdown files (relative to docs_dir)."""
    active_docs = []
    for root, dirs, files in os.walk(docs_dir):
        rel_root = os.path.relpath(root, docs_dir)
        parts = rel_root.split(os.sep) if rel_root != "." else []
        if any(ex in parts for ex in EXCLUDE_DIRS):
            continue
        for f in files:
            if f.endswith(".md"):
                p = os.path.relpath(os.path.join(root, f), docs_dir)
                active_docs.append(p)
    return sorted(active_docs)


def build_headings_map(docs_dir: str, active_docs: list) -> dict:
    """Build a mapping of doc_rel -> set of valid anchor slugs and raw titles."""
    headings_map = {}
    for doc in active_docs:
        full_path = os.path.join(docs_dir, doc)
        with open(full_path, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()
        slugs = set()
        for m in HEADING_PATTERN.finditer(content):
            raw_h = m.group(2).strip()
            clean_h = re.sub(r"[`*_]", "", raw_h)
            slugs.add(slugify(clean_h))
            slugs.add(slugify(raw_h))
            slugs.add(clean_h.lower())
        headings_map[doc] = slugs
    return headings_map


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-v", "--verbose", action="store_true", help="Print detailed diagnostic progress")
    args = parser.parse_args()

    errors = []

    # 1. Parse MkDocs nav
    nav_files = parse_mkdocs_nav(MKDOCS_FILE)
    if args.verbose:
        print(f"Parsed {len(nav_files)} navigation entries from site/mkdocs.yml")

    # 2. Check nav targets exist
    for f in sorted(nav_files):
        target = os.path.join(DOCS_DIR, f)
        if not os.path.exists(target):
            errors.append(f"MkDocs nav points to missing file: docs/{f}")

    # 3. Check 100% nav coverage for active docs
    active_docs = collect_active_docs(DOCS_DIR)
    unmapped = [f for f in active_docs if f not in nav_files]
    for u in unmapped:
        errors.append(f"Active doc missing from site/mkdocs.yml: docs/{u}")

    # 4. Check relative links and anchors
    headings_map = build_headings_map(DOCS_DIR, active_docs)
    broken_links = 0
    broken_anchors = 0

    for doc in active_docs:
        full_path = os.path.join(DOCS_DIR, doc)
        doc_dir = os.path.dirname(full_path)
        with open(full_path, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()

        for text, url in LINK_PATTERN.findall(content):
            url = url.strip()
            if (url.startswith("http://") or url.startswith("https://") or
                    url.startswith("mailto:") or url.startswith("file://")):
                continue

            # Split path and anchor
            if "#" in url:
                path_part, anchor = url.split("#", 1)
                anchor = anchor.strip()
            else:
                path_part, anchor = url, None

            # Check relative file existence
            if path_part and path_part != ".":
                target_full = os.path.normpath(os.path.join(doc_dir, path_part))
                if not os.path.exists(target_full):
                    errors.append(f"{doc}: Broken link -> '{url}' (target does not exist)")
                    broken_links += 1
                    continue
                target_doc = os.path.relpath(target_full, DOCS_DIR)
            else:
                target_doc = doc

            # Check anchor validity if target is an active markdown doc
            if anchor and target_doc in headings_map:
                valid_slugs = headings_map[target_doc]
                norm_anchor = slugify(anchor)
                matched = any(norm_anchor in s or s in norm_anchor for s in valid_slugs)
                if not matched:
                    errors.append(f"{doc}: Broken anchor -> '{url}' (target '{target_doc}' has no matching heading)")
                    broken_anchors += 1

    if errors:
        print(f"check-doc-nav: FAILED ({len(errors)} errors found)", file=sys.stderr)
        for err in errors:
            print(f"  ERROR: {err}", file=sys.stderr)
        sys.exit(1)

    print(f"check-doc-nav: {len(active_docs)} active docs in nav (100% coverage), "
          f"{len(nav_files)} nav links verified, 0 broken links, 0 broken anchors: PASS")


if __name__ == "__main__":
    main()
