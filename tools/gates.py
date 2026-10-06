#!/usr/bin/env python3
"""Run the source-level contract assertions in tools/gates.toml.

These checks used to be ~97 inline `rg -q` recipes in tools/targets-gates.mk.
They are assertions about what appears in which file, so the natural shape is a
table (tools/gates.toml) plus one runner, not 97 shell lines.

`rg -q` is silent on success and on failure alike, which makes a failing gate
unhelpful: you learn that `rg` returned 1 but not what it found.  So a failing
assertion is re-run without `-q` to print the matching lines, and the gate names
the assertion that broke.  The pass/fail decision is identical to the recipes.
"""

from __future__ import annotations

import argparse
import glob as globlib
import os
import re
import subprocess
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOML = Path(__file__).resolve().parent / "gates.toml"

# rg exit codes: 0 = match, 1 = no match, 2 = error (missing file, bad regex).
RC_MATCH, RC_NOMATCH, RC_ERROR = 0, 1, 2


def load_gates() -> dict[str, dict]:
    doc = tomllib.loads(TOML.read_text(encoding="utf-8"))
    return {g["name"]: g for g in doc["gate"]}


def expand(files: list[str]) -> list[str]:
    """Expand `*` in a path, the way the shell did when these were recipes.

    Kept as expansion rather than a baked file list so that dropping a new
    source file into a checked directory is covered without editing the table.
    A glob that matches nothing is an error, mirroring the shell passing the
    unexpanded word to rg, which then failed.
    """
    out: list[str] = []
    for f in files:
        if any(c in f for c in "*?["):
            hits = sorted(globlib.glob(f, root_dir=REPO))
            if not hits:
                raise SystemExit(f"error: pattern {f!r} matched no files")
            out.extend(hits)
        else:
            out.append(f)
    return out


def _run_build_must_fail(a: dict) -> tuple[bool, str]:
    """Assert that `make ARCH=<arch> NOMMU=1 <target>` is REJECTED.

    The polarity is inverted on purpose: these architectures must refuse a
    NOMMU build, so a zero exit status is the failure.  make rejects them in its
    variable-validation stage (Makefile:115), before invoking a compiler, so no
    cross toolchain is needed to run this.
    """
    spec = a["build_must_fail"]
    r = subprocess.run(["make", "-s", f"ARCH={spec['arch']}", "NOMMU=1",
                        spec["target"]], cwd=REPO, check=False,
                       capture_output=True, text=True)
    if r.returncode == 0:
        return False, (f"unsupported NOMMU build accepted for {spec['arch']}")
    return True, ""


def run_one(a: dict, files: list[str]) -> tuple[bool, str]:
    """Return (ok, detail).  detail is empty when ok.

    `rg_flags` carries the flags the original recipe needed.  Two of them are
    load-bearing rather than cosmetic: `--pcre2` is required by the lookaround
    patterns (rg's default Rust-regex engine cannot compile them), and `-U` is
    required by the patterns that span lines, since without it rg matches
    per-line and can never match at all.
    """
    if a.get("exists"):
        return _run_exists(a)
    if a.get("build_must_fail"):
        return _run_build_must_fail(a)
    if a.get("doc_refs"):
        return _run_doc_refs(a)
    if a.get("native_abi_coverage"):
        return _run_native_abi_coverage(a)

    argv = ["rg", "-q", *a.get("rg_flags", ())]
    if a.get("fixed"):
        argv.append("-F")
    # Globs must precede the path: rg treats a `--glob` that appears after a
    # positional argument as another path, silently dropping the filter
    # (measured: 23 matches with the glob last, 17 with it first).
    for g in a.get("globs", [a["glob"]] if a.get("glob") else []):
        argv += ["--glob", g]
    # A pattern may start with '-' (e.g. '->state'); rg then needs `--` or it
    # reads the pattern as a flag.
    if a.get("ddash"):
        argv.append("--")
    argv.append(a["pattern"])
    argv.extend(files)

    if a.get("post_filter"):
        return _run_post_filtered(a, argv)

    r = subprocess.run(argv, cwd=REPO, check=False, capture_output=True, text=True)
    negate = bool(a.get("negate", False))
    min_count = a.get("min_count")

    if r.returncode == RC_ERROR:
        return False, f"rg error: {r.stderr.strip()}"

    if min_count is not None and not negate:
        # A bare `pattern` presence check cannot see a *removed* call site: the
        # name still appears somewhere else in the file, so the gate stays green
        # through exactly the change it exists to catch. Counting matches is the
        # only way a presence assertion can notice one fewer lock.
        want = int(min_count)
        count_argv = ["rg", "-c", *a.get("rg_flags", ())]
        if a.get("fixed"):
            count_argv.append("-F")
        for g in a.get("globs", [a["glob"]] if a.get("glob") else []):
            count_argv += ["--glob", g]
        if a.get("ddash"):
            count_argv.append("--")
        count_argv.append(a["pattern"])
        count_argv.extend(files)
        c = subprocess.run(count_argv, cwd=REPO, check=False,
                           capture_output=True, text=True)
        if c.returncode == RC_ERROR:
            return False, f"rg error: {c.stderr.strip()}"
        total = 0
        for line in c.stdout.splitlines():
            # `rg -c` prints a bare count for a single file and `file:count` for
            # several, so both forms have to be accepted or the total silently
            # reads as zero and the assertion can never be satisfied.
            head, sep, tail = line.rpartition(":")
            n = tail if sep and head else line
            if n.strip().isdigit():
                total += int(n)
        if total < want:
            return False, (f"pattern {a['pattern']!r} matched {total} time(s) in "
                           f"{', '.join(files)}, need at least {want}")
        return True, ""

    matched = r.returncode == RC_MATCH
    if matched == negate:
        # Positive + no match, or negated + match: the assertion is violated.
        if negate:
            show = ["rg", "-n", *a.get("rg_flags", ())]
            for g in a.get("globs", [a["glob"]] if a.get("glob") else []):
                show += ["--glob", g]
            if a.get("ddash"):
                show.append("--")
            show += [a["pattern"], *files]
            shown = subprocess.run(show, cwd=REPO, check=False,
                                   capture_output=True, text=True)
            return False, (f"pattern {a['pattern']!r} must not appear in "
                           f"{', '.join(files)}\n{shown.stdout}")
        return False, f"pattern {a['pattern']!r} not found in {', '.join(files)}"
    return True, ""


def _run_exists(a: dict) -> tuple[bool, str]:
    """`test -r <file> && ...`: every listed path must be a readable file.

    Kept as a table entry rather than left in the recipe so the gate can stay a
    single delegation: the original interleaves these existence guards between
    rg assertions, and moving only the rg lines would reorder the checks.
    """
    missing = [f for f in a["exists"] if not os.access(REPO / f, os.R_OK)]
    if missing:
        return False, "missing required file(s): " + ", ".join(missing)
    return True, ""


# A doc may legitimately name a source file that does not exist: it was
# deleted, it is planned, it is a build-generated path, or the sentence is
# telling the reader to create it.  The docs already say so in Chinese at the
# point of reference, so the gate reads that rather than requiring a second
# hand-maintained list, which would rot.
_REF = re.compile(
    r"`((?:kernel|user|tools|packages|instances|components)/"
    r"[A-Za-z0-9_./-]+\.(?:c|h|S|ld|mk|py|toml|md))(?::(\d+))?`")
# Per-line: this reference is flagged as historical or hypothetical.
_REF_MARKERS = ("已删除", "已退役", "已移除", "已迁移", "移除", "删除", "退役",
                "不存在", "已取代", "曾用", "原内建", "规划", "尚未", "planned",
                "retired", "deleted", "will be", "为例，新建", "举例",
                "请新建", "自行创建")
# Document-level: the whole page is an archive, so every ref in it is exempt.
_DOC_MARKERS = ("已完全退役", "不是当前", "已归档", "历史")


def _run_doc_refs(a: dict) -> tuple[bool, str]:
    """Every source path a doc cites must still exist, or be marked exempt.

    Keyword-presence assertions cannot see this class of drift: a renamed file,
    a moved function and a stale complexity claim all leave the prose intact.
    A keyword gate stays green through exactly the changes it exists to catch.

    Scoped to fully-qualified paths only.  A bare `park.c` in prose is
    shorthand, not a link, and there are hundreds of those; requiring them to
    resolve would bury the signal.  Line numbers are checked against the real
    file length, so a ref cannot outlive the code it points into.

    A reference that does not resolve is only a failure when nothing marks it
    as historical.  That keeps the gate self-maintaining: the annotation is
    already what a careful author writes, and this makes it load-bearing.
    """
    roots = a.get("doc_roots", ["docs"])
    skip = [f"{r}/" for r in a.get("doc_skip", ["docs/archive"])]
    broken: list[str] = []
    total = 0
    for root in roots:
        base = REPO / root
        for path in sorted(base.rglob("*.md")):
            rel = str(path.relative_to(REPO))
            if any(rel.startswith(s) for s in skip):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            lines = text.splitlines()
            doc_exempt = any(m in "\n".join(lines[:14]) for m in _DOC_MARKERS)
            for lineno, line in enumerate(lines, 1):
                for m in _REF.finditer(line):
                    total += 1
                    ref, cited = m.group(1), m.group(2)
                    target = REPO / ref
                    if target.exists():
                        if cited:
                            n = len(target.read_text(
                                encoding="utf-8", errors="replace").splitlines())
                            if int(cited) > n:
                                broken.append(
                                    f"{rel}:{lineno}: {ref}:{cited} but the "
                                    f"file has {n} lines")
                        continue
                    if doc_exempt or any(k in line for k in _REF_MARKERS):
                        continue
                    broken.append(f"{rel}:{lineno}: {ref} does not exist "
                                  f"and is not marked as historical")
    if broken:
        return False, (f"{len(broken)}/{total} cited source paths do not "
                       f"resolve:\n" + "\n".join(broken[:20]))
    return True, ""


_NR_DEFINE = re.compile(r"^#define A20_SYS_([A-Za-z0-9_]+)\s+(0x[0-9A-Fa-f]+)\s*$",
                        re.M)
_SYSCALL_ROW = re.compile(r"^\|\s*(0x[0-9A-Fa-f]+)\s*\|\s*`([A-Za-z0-9_]+)`", re.M)
_HEADING = re.compile(r"^#{1,%(level)d}(\s|$)")


def _section_span(text: str, heading: str) -> str | None:
    """The text from `heading` up to the next heading of the same level or above.

    Anchored on the heading text rather than a line number so that inserting a
    section upstream cannot silently move the region this gate reads.  The body
    is included: the syscall list is a `##` whose rows sit under `###` range
    headings, so stopping at the first *deeper* heading would leave an empty
    span and make every row assertion below vacuously true.
    """
    lines = text.splitlines(keepends=True)
    start = next((i for i, ln in enumerate(lines) if ln.rstrip() == heading), None)
    if start is None:
        return None
    level = len(lines[start]) - len(lines[start].lstrip("#"))
    stop = next((j for j in range(start + 1, len(lines))
                 if _HEADING.match(lines[j], 0, level)), len(lines))
    return "".join(lines[start:stop])


def _run_native_abi_coverage(a: dict) -> tuple[bool, str]:
    """Cross-check the Native ABI's three tables against each other.

    The Linux side has two coverage generators, so a new LINUX_SYSCALL needs a
    coverage row and an envelope classification.  The Native side had neither:
    `check-abi-boundary` was 19 single-file keyword checks, so a native entry
    could be registered with no number, with a number the spec does not list, or
    with no spec row at all and every gate stayed green -- which is how two
    defective native entries reached main.  Nothing here re-derives a judgement:
    it only asks that the sources agree, and names the entries that do not, so
    the "nobody ever decided" case is a gate failure rather than an absence
    nobody can see.  "Named somewhere under docs/native-abi/" is a weaker
    statement than "has a row in the syscall list"; the list is checked in both
    directions, and the SDK mirror is checked name-and-number, so a number can
    no longer be correct in the kernel header yet unreachable from user space.
    """
    spec = a["native_abi_coverage"]
    names = re.findall(r"^A20_NATIVE_SYSCALL\(\s*([A-Za-z0-9_]+)\s*,",
                       (REPO / spec["table"]).read_text(encoding="utf-8"), re.M)
    numbers = {n: int(v, 16) for n, v in
               _NR_DEFINE.findall((REPO / spec["numbers"]).read_text(encoding="utf-8"))}

    problems: list[str] = []
    if not names:
        problems.append(f"no A20_NATIVE_SYSCALL entries parsed from {spec['table']}")
    unnumbered = [n for n in names if n not in numbers]
    unregistered = [n for n in numbers if n not in names]
    if unnumbered:
        problems.append(f"{len(unnumbered)}/{len(names)} entries in {spec['table']} "
                        f"have no A20_SYS_ number in {spec['numbers']}: "
                        f"{', '.join(unnumbered)}")
    if unregistered:
        problems.append(f"{len(unregistered)} A20_SYS_ numbers in {spec['numbers']} "
                        f"have no entry in {spec['table']}: {', '.join(unregistered)}")

    docs = sorted((REPO / spec["doc_dir"]).rglob("*.md"))
    prose = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in docs)
    undocumented = [n for n in names
                    if not re.search(r"(?<![0-9A-Za-z_])" + re.escape(n) +
                                     r"(?![0-9A-Za-z_])", prose)]
    if undocumented:
        problems.append(f"{len(undocumented)}/{len(names)} entries in "
                        f"{spec['table']} are named nowhere in {spec['doc_dir']}/ "
                        f"({len(docs)} files): {', '.join(undocumented)}")

    listed = spec["syscall_list"]
    section = _section_span((REPO / listed["doc"]).read_text(encoding="utf-8"),
                            listed["heading"])
    if section is None:
        problems.append(f"{listed['doc']} has no {listed['heading']!r} heading")
    else:
        rows = _SYSCALL_ROW.findall(section)
        for num, name in rows:
            if name not in numbers:
                problems.append(f"{listed['doc']} {listed['heading']} lists {name}, "
                                f"which has no A20_SYS_ number")
            elif numbers[name] != int(num, 16):
                problems.append(f"{listed['doc']} lists {name} as {num}, "
                                f"{spec['numbers']} says {numbers[name]:#06x}")
        listed_names = {name for _, name in rows}
        unlisted = sorted(set(numbers) - listed_names)
        if unlisted:
            problems.append(f"{len(unlisted)}/{len(numbers)} numbers in "
                            f"{spec['numbers']} have no row in {listed['doc']} "
                            f"{listed['heading']}: {', '.join(unlisted)}")
        claimed = re.search(r"总计[：:]\s*(\d+)", section)
        if claimed and int(claimed.group(1)) != len(numbers):
            problems.append(f"{listed['doc']} {listed['heading']} claims "
                            f"{claimed.group(1)} syscalls but "
                            f"{spec['numbers']} registers {len(numbers)}")

    sdk = spec.get("sdk_numbers")
    if sdk:
        sdk_numbers = {n: int(v, 16) for n, v in
                       _NR_DEFINE.findall((REPO / sdk).read_text(encoding="utf-8"))}
        drifted = [f"{n}={v:#06x} vs {numbers[n]:#06x}"
                   for n, v in sorted(sdk_numbers.items())
                   if n not in numbers or numbers[n] != v]
        if drifted:
            problems.append(f"{len(drifted)}/{len(sdk_numbers)} numbers in {sdk} "
                            f"disagree with {spec['numbers']}: "
                            f"{', '.join(drifted)}")
        absent = sorted(set(numbers) - set(sdk_numbers))
        if absent:
            problems.append(f"{len(absent)}/{len(numbers)} numbers in "
                            f"{spec['numbers']} are missing from {sdk}, so a user "
                            f"program including it cannot name them: "
                            f"{', '.join(absent)}")

    dupes = sorted({v for v in numbers.values()
                    if list(numbers.values()).count(v) > 1})
    if dupes:
        problems.append(f"{len(dupes)} number(s) are shared by more than one "
                        f"entry in {spec['numbers']}: "
                        f"{', '.join(f'{v:#06x}' for v in dupes)}")

    if problems:
        return False, "\n".join(problems)
    return True, ""


def _run_post_filtered(a: dict, argv: list[str]) -> tuple[bool, str]:
    """`bad=$(rg ... | rg -v WHITELIST)`; require nothing to survive the filter.

    These assertions cannot be expressed as an rg exit code: the whitelist drops
    the files allowed to contain the pattern, so the verdict depends on the
    surviving *output*, which has to be captured and filtered here.
    """
    r = subprocess.run(["rg", "-n", *argv[2:]], cwd=REPO, check=False,
                       capture_output=True, text=True)
    if r.returncode not in (0, 1):
        return False, f"rg error: {r.stderr.strip()}"
    keep = [ln for ln in r.stdout.splitlines()
            if not re.search(a["post_filter"], ln)]
    if keep:
        return False, (f"pattern {a['pattern']!r} appears outside the allowed "
                       f"files:\n" + "\n".join(keep[:20]))
    return True, ""


def run_gate(name: str, gate: dict, segment: int | None = None) -> int:
    files_cache: dict[tuple, list[str]] = {}
    failures: list[str] = []
    nseg = gate.get("segments", 1)
    items = gate["assertion"]
    if segment is not None:
        # A gate whose assertions straddle a codegen step is split into
        # segments so make can run the generator between two calls and the
        # original check order is preserved.
        if not 0 <= segment < nseg:
            raise SystemExit(f"error: segment {segment} out of range (0..{nseg - 1})")
        items = [a for a in items if a.get("segment", 0) == segment]
    for a in items:
        # `exists` assertions name their paths in `exists`, never `files`.
        key = tuple(a.get("files", ()))
        if key and key not in files_cache:
            try:
                files_cache[key] = expand(a["files"])
            except SystemExit as exc:
                failures.append(str(exc))
                files_cache[key] = []
        ok, detail = run_one(a, files_cache.get(key, []))
        if not ok:
            failures.append(detail)

    total = len(items)
    label = gate["make_target"]
    if segment is not None:
        label = f"{label}[{segment}]"
    if failures:
        print(f"{label}: {len(failures)}/{total} assertions FAILED", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"{label}: {total} assertions PASS")
    return 0


def run_host_tests(binaries: list[str]) -> int:
    """Run each host-compiled unit test binary, stopping at the first failure.

    make owns the compilation (HOST_CC, HOST_CFLAGS, the pattern rule); running
    the results is orchestration, so it lives here.  The binaries come from
    make as arguments rather than being re-globbed here, so the set that runs is
    exactly the set make decided was out of date.
    """
    for t in binaries:
        print(f"== {t} ==", flush=True)
        r = subprocess.run([t], cwd=REPO, check=False)
        if r.returncode != 0:
            print(f"host-tests: {t} failed with status {r.returncode}",
                  file=sys.stderr)
            return r.returncode
    print("host-tests: PASS")
    return 0


def run_whitespace(label: str) -> int:
    """`git diff --check`: fail on whitespace errors in the working diff.

    The six check-proc-step*-local targets each ran this after their source
    assertions, so it is a real gate rather than build noise.
    """
    r = subprocess.run(["git", "diff", "--check"], cwd=REPO, check=False,
                       capture_output=True, text=True)
    if r.returncode != 0 or r.stdout.strip():
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        print(f"{label}: whitespace errors in the working diff", file=sys.stderr)
        return 1
    print(f"{label}: PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("gate",
                    help="gate name from tools/gates.toml, 'host-tests' or "
                         "'whitespace'")
    ap.add_argument("--list", action="store_true", help="list gate names and exit")
    ap.add_argument("--binaries", default="",
                    help="space-separated host test binaries (host-tests only)")
    ap.add_argument("--segment", type=int, default=None,
                    help="run only this contiguous run of a split gate")
    ap.add_argument("--label", default="whitespace",
                    help="label for the PASS line (whitespace only)")
    a = ap.parse_args()

    if a.gate == "host-tests":
        binaries = a.binaries.split()
        if not binaries:
            raise SystemExit("error: host-tests needs --binaries")
        return run_host_tests(binaries)
    if a.gate == "whitespace":
        return run_whitespace(a.label)

    gates = load_gates()
    if a.list:
        for name, g in gates.items():
            print(f"{name:32} {len(g['assertion']):3} assertions  ({g['make_target']})")
        return 0
    if a.gate not in gates:
        raise SystemExit(f"error: unknown gate {a.gate!r}; known: "
                         f"{', '.join(sorted(gates))}")
    return run_gate(a.gate, gates[a.gate], a.segment)


if __name__ == "__main__":
    raise SystemExit(main())
