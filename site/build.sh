#!/usr/bin/env bash
# Assemble the landing page and bilingual documentation for GitHub Pages.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
command -v mkdocs >/dev/null 2>&1 || {
    echo "error: mkdocs not found; install with: pip install -r site/requirements.txt" >&2
    exit 1
}
OUT="$ROOT/build/docs/site"
mkdir -p "$OUT"
mkdocs build -f mkdocs.yml -d "$OUT/docs"
cp -a site/landing/. "$OUT/"
echo "site assembled at build/docs/site/ ($(find "$OUT" -type f | wc -l) files)"
