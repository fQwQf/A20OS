#!/usr/bin/env python3
"""Build the exact-execution corpus staging tree for the A20OS guest.

Takes the extracted DataDog samples and produces a staging directory
that becomes a FAT32 data disk: two per-arm copies of every sample
(corpus_none/, corpus_env/), one run script per sample (run/<key>.sh),
and a run_all.sh driver.  Filenames are sanitized for FAT32.

Usage: gen_exec_corpus.py <extracted-root> <staging-dir>
"""
import json
import os
import re
import shutil
import sys

LIFECYCLE = ["preinstall", "install", "postinstall", "prepare"]


def sanitize(name):
    return re.sub(r"[^A-Za-z0-9._@-]", "_", name)


def copy_sanitized(src, dst):
    """Copy a tree, sanitizing path components for FAT32."""
    n = 0
    for dirpath, dirs, files in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        parts = [] if rel == "." else [sanitize(p) for p in rel.split(os.sep)]
        tdir = os.path.join(dst, *parts)
        os.makedirs(tdir, exist_ok=True)
        dirs[:] = [d for d in dirs if not d.startswith("package_info")]
        for f in files:
            if f.startswith("package_info"):
                continue
            s = os.path.join(dirpath, f)
            t = os.path.join(tdir, sanitize(f))
            if os.path.getsize(s) > 8 << 20:
                continue
            shutil.copy2(s, t)
            n += 1
    return n


def find_pkg_dir(root, marker):
    for dirpath, _dirs, files in os.walk(root):
        if marker in files:
            return os.path.relpath(dirpath, root)
    return None


def npm_scripts(pkg_json):
    try:
        with open(pkg_json) as f:
            data = json.load(f)
    except Exception:
        return []
    scripts = data.get("scripts") or {}
    return [(k, scripts[k]) for k in LIFECYCLE
            if isinstance(scripts.get(k), str) and scripts[k].strip()]


def main():
    src_root, staging = sys.argv[1], sys.argv[2]
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(os.path.join(staging, "run"))

    stats = {"npm": 0, "pypi": 0, "no_entry": [], "samples": []}
    run_all_lines = []

    for eco in ("npm", "pypi"):
        eco_dir = os.path.join(src_root, eco)
        if not os.path.isdir(eco_dir):
            continue
        for name in sorted(os.listdir(eco_dir)):
            src = os.path.join(eco_dir, name)
            if not os.path.isdir(src):
                continue
            key = sanitize(f"{eco}__{name}")[:80]
            marker = "package.json" if eco == "npm" else "setup.py"

            entry = None
            for arm in ("none", "env"):
                dst = os.path.join(staging, f"corpus_{arm}", key)
                copy_sanitized(src, dst)
            pkg_rel = find_pkg_dir(os.path.join(staging, "corpus_none", key),
                                   marker)
            if pkg_rel is None:
                stats["no_entry"].append(key)
                continue

            if eco == "npm":
                scripts = npm_scripts(os.path.join(
                    staging, "corpus_none", key, pkg_rel, "package.json"))
                if not scripts:
                    stats["no_entry"].append(key)
                    continue
                body = [f'cd "$1/{key}/{pkg_rel}" || exit 3']
                for i, (_hook, cmd) in enumerate(scripts):
                    body.append(f"sh -s <<'A20EOF_{i}'\n{cmd}\nA20EOF_{i}")
                entry = "\n".join(body) + "\n"
            else:
                body = [
                    f'cd "$1/{key}/{pkg_rel}" || exit 3',
                    f'python3 setup.py install --prefix=/tmp/pfx_{key}',
                ]
                entry = "\n".join(body) + "\n"

            with open(os.path.join(staging, "run", key + ".sh"), "w") as f:
                f.write(entry)
            stats[eco] += 1
            stats["samples"].append(key)

    run_all = """#!/bin/sh
# Exact-execution corpus driver: each sample runs twice --
# once bare (NONE), once under an install-time envelope (ENV).
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
echo "CORPUS_EXEC: start"
N=0
for s in /samples/run/*.sh; do
  n=$(basename "$s" .sh)
  echo "=== SAMPLE $n"
  timeout 30 sh "$s" /samples/corpus_none > /tmp/o_none 2>&1
  echo "NONE-EXIT=$?"
  grep -oaE "EPERM|EACCES|EAI_[A-Z_]*|EHOSTUNREACH|ENETUNREACH|ECONNREFUSED|ETIMEDOUT" /tmp/o_none | head -3 | tr '\\n' ' '
  echo "<-none-sig"
  timeout 30 /samples/envwrap sh "$s" /samples/corpus_env > /tmp/o_env 2>&1
  echo "ENV-EXIT=$?"
  grep -oaE "EPERM|EACCES|EAI_[A-Z_]*|EHOSTUNREACH|ENETUNREACH|ECONNREFUSED|ETIMEDOUT" /tmp/o_env | head -3 | tr '\\n' ' '
  echo "<-env-sig"
  N=$((N+1))
done
echo "CORPUS_EXEC: done n=$N"
"""
    with open(os.path.join(staging, "run_all.sh"), "w") as f:
        f.write(run_all)

    print(json.dumps({k: v for k, v in stats.items() if k != "samples"},
                     indent=1))
    print(f"run-scripts={len(stats['samples'])}")


if __name__ == "__main__":
    main()
