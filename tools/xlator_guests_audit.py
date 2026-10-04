#!/usr/bin/env python3
"""Check that the three guest-registry facts stay in agreement.

"Adding a guest architecture" is meant to cost two edits and no thought:

    kernel/proc/xlator_guests.def      -- what the kernel will translate
    tools/targets-xlator.mk            -- which cross compiler builds the probe

Those two *cannot* be derived from one another: the .def is kernel policy and
the CC map is a fact about which gcc happens to be installed on the build
host.  So the link between them has to be asserted instead of computed, and
this is that assertion.  It already has a reason to exist -- the CC map
carried an `XLATOR_GUEST_CC_riscv64` line for a guest the kernel had never
heard of, which read as supported while `--guest riscv64` was rejected by
argparse.

Four things are checked:

  1. every guest in the .def has an XLATOR_GUEST_CC_<guest> assignment
  2. every XLATOR_GUEST_CC_<guest> assignment has a guest in the .def
     (the drift above; an unused CC line is a promise nobody keeps)
  3. every e_machine in the .def equals the EM_* constant of the same name
     in kernel/include/mm/elf.h
  4. every argv template in the .def is well-formed: it names only the
     three substitutions the kernel knows, and it contains @P
  5. every architecture in XLATOR_SUPPORTED_ARCHES defines
     arch_bootargs_get, so a.xlator-capable arch can actually be
     *configured* -- see check_bootargs_coverage() for why that is a
     separate fact from being compiled in

Plus a scan for a *third* guest list.  The whole point of the .def is that
there is exactly one, and a resurrected GUEST_TARGETS dict in the fetch tool
would be invisible to everything else -- argparse would happily accept a
guest the kernel cannot translate.

Host-side pure text, so it belongs in CHECK_FAST_GATES: no cross toolchain,
no QEMU.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

GUESTS_DEF = REPO / "kernel" / "proc" / "xlator_guests.def"
XSLATOR_MK = REPO / "tools" / "targets-xlator.mk"
ELF_H = REPO / "kernel" / "include" / "mm" / "elf.h"
FETCH_PY = REPO / "tools" / "xlator_fetch.py"
MAKEFILE = REPO / "Makefile"
KERNEL_DIR = REPO / "kernel"
WEAK_BOOTARGS = "core/bootargs.c"

XLATOR_ARCHES_RE = re.compile(r"^XLATOR_SUPPORTED_ARCHES\s*:?=\s*(.*)$", re.M)
BOOTARGS_DEF_RE = re.compile(r"^\s*(?:__attribute__\(\(weak\)\)\s*)?"
                             r"(?:const\s+char\s*\*|\w[\w \t*]*)\s*"
                             r"arch_bootargs_get\s*\(",
                             re.M)

# XLATOR_GUEST(name, e_machine, "argv_template").  The trailing column is
# captured too, so it can be validated; the C string is unquoted and
# whitespace-normalised before the token check.
GUEST_RE = re.compile(
    r"^\s*XLATOR_GUEST\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)\s*,"
    r"\s*\"([^\"]*)\"\s*\)", re.M)

# The kernel's whole template vocabulary (kernel/proc/xlator.c,
# xlator_parse_template).  A '@' followed by anything else is rejected there,
# so a template using one is a boot-time warning at best.
XLATOR_TOKENS = {"@A", "@P", "@*"}

CC_RE = re.compile(r"^XLATOR_GUEST_CC_([A-Za-z_][A-Za-z0-9_]*)\s*:?=", re.M)

EM_RE = re.compile(r"#define\s+(EM_[A-Z0-9_]+)\s+(\d+)")


def fail(msg: str) -> int:
    print(f"check-xlator-guests: FAIL -- {msg}")
    return 1


def parse_xlator_arches() -> list[str]:
    """The architectures the Makefile compiles CONFIG_XLATOR into."""
    m = XLATOR_ARCHES_RE.search(MAKEFILE.read_text())
    if not m:
        raise ValueError(f"no XLATOR_SUPPORTED_ARCHES in {MAKEFILE.name}")
    return m.group(1).split()


def archs_defining_bootargs() -> dict[str, list[str]]:
    """Map each architecture name to the files that implement its hook.

    Ownership is taken from the path, not from a table: the implementations
    live either under kernel/arch/<arch>/ or in a board directory named for
    the architecture (kernel/platform/qemu-virt-aarch64/board.c), so the name
    appearing in the path is what ties a definition to an architecture.

    kernel/core/bootargs.c is skipped on purpose.  Its weak default *is* the
    bug this looks for, so counting it would make the check vacuous.
    """
    owners: dict[str, list[str]] = {}
    for path in sorted(KERNEL_DIR.rglob("*.c")):
        rel = path.relative_to(KERNEL_DIR).as_posix()
        if rel == WEAK_BOOTARGS:
            continue
        if not BOOTARGS_DEF_RE.search(path.read_text(errors="replace")):
            continue
        for arch in parse_xlator_arches():
            if arch in rel:
                owners.setdefault(arch, []).append(rel)
    return owners


def check_bootargs_coverage() -> str | None:
    """Every xlator-capable arch must be able to receive a command line.

    Being listed in XLATOR_SUPPORTED_ARCHES only means CONFIG_XLATOR is
    compiled in.  Whether the channel can be *configured* is a separate fact:
    the only configuration input is bootargs_get(), and an architecture with
    no arch_bootargs_get() falls through to the weak default in
    kernel/core/bootargs.c that returns NULL.  Every `a20.*` key then reads
    as absent on that architecture -- not just a20.xlator, but a20.ip,
    a20.tcpmode, a20.wx and the rest -- while the kernel still reports the
    feature as built in.

    loongarch64 was exactly this for the whole life of the channel: it is in
    XLATOR_SUPPORTED_ARCHES, so CONFIG_XLATOR compiled and /proc/a20/xlator
    was registered, and no a20.* key could ever take effect because nothing
    ever populated the command line.

    An explicit stub that returns NULL is still allowed -- arm32 and
    loongarch32 have one on purpose, having no FDT path at all.  What this
    rejects is the *silent* fallback: an architecture that never made the
    decision.
    """
    owners = archs_defining_bootargs()
    missing = [a for a in parse_xlator_arches() if not owners.get(a)]
    if missing:
        subject = (f"{missing[0]} is" if len(missing) == 1
                   else f"{', '.join(missing)} are")
        return (f"{subject} in XLATOR_SUPPORTED_ARCHES but "
                f"define{'s' if len(missing) == 1 else ''} no "
                f"arch_bootargs_get(), so the weak default in "
                f"kernel/{WEAK_BOOTARGS} returns NULL and no a20.* key can "
                f"take effect there.  Implement it (an FDT /chosen/bootargs "
                f"reader is what every other MMU arch does), or -- if the "
                f"architecture genuinely has no command line -- write an "
                f"explicit stub returning NULL and drop it from "
                f"XLATOR_SUPPORTED_ARCHES.")
    return None


def parse_guests() -> dict[str, tuple[int, str]]:
    out: dict[str, tuple[int, str]] = {}
    for m in GUEST_RE.finditer(GUESTS_DEF.read_text()):
        name, machine, tmpl = m.group(1), int(m.group(2)), m.group(3)
        if name in out:
            raise ValueError(f"{name} listed twice in {GUESTS_DEF.name}")
        out[name] = (machine, tmpl)
    if not out:
        raise ValueError(f"no XLATOR_GUEST rows in {GUESTS_DEF.name}")
    return out


def check_template(name: str, tmpl: str) -> str | None:
    """Return why @tmpl is unusable, or None when it is fine.

    A cheap re-statement of kernel/proc/xlator.c's tokenizer, not a second
    implementation of it: the kernel is the authority and enforces all of
    this at boot, where the failure is a warning and an unconfigured guest.
    The gate exists so the mistake is caught on the host in a second rather
    than in a boot log on the target.  The two must be kept in step -- a new
    substitution means adding it to XLATOR_TOKENS here too.
    """
    tokens = [t for t in re.split(r"[\s,]+", tmpl) if t]
    if not tokens:
        return "is empty"
    if "@P" not in tokens:
        # @P may legitimately be glued to a literal ("--argv0=@A"), so look
        # for it as a substring before giving up.
        if "@P" not in tmpl:
            return "does not mention @P, so it names no image to translate"
    for tok in tokens:
        for i, ch in enumerate(tok):
            if ch != "@":
                continue
            sub = tok[i:i + 2]
            if sub not in XLATOR_TOKENS:
                return f"uses {sub!r}, which is not one of {sorted(XLATOR_TOKENS)}"
    return None


def parse_cc_map() -> set[str]:
    return set(CC_RE.findall(XSLATOR_MK.read_text()))


def parse_elf_machines() -> dict[str, int]:
    return {m.group(1): int(m.group(2))
            for m in EM_RE.finditer(ELF_H.read_text())}


def find_stray_guest_lists() -> list[str]:
    """Report a second hardcoded guest->e_machine table in tools/.

    Deliberately narrow.  Two other tables in tools/ legitimately map an
    architecture name to an e_machine and must not be mistaken for a third
    guest list:

      * xlator_fetch.py's HOST_MACHINES -- the architectures a *translator*
        may be built for.  That is a property of the build host, not of the
        guest registry.
      * mkrootfs.py's arch map -- which Alpine architecture a rootfs package
        tree is fetched for.  Unrelated to translation.

    So the signal is not the numbers, it is the *name*: a dict literal bound
    to a name mentioning GUEST, outside the .def and outside this file.
    """
    suspects = []
    pattern = re.compile(r"^(?:\w*GUEST\w*)\s*(?::[^=]*)?=\s*\{\s*$", re.M)
    for path in sorted((REPO / "tools").glob("*.py")):
        if path.name == Path(__file__).name:
            continue
        for i, line in enumerate(path.read_text().splitlines(), 1):
            if pattern.search(line):
                suspects.append(f"{path.relative_to(REPO)}:{i}: {line.strip()}")
    return suspects


def main() -> int:
    try:
        guests = parse_guests()
        elf_machines = parse_elf_machines()
    except (OSError, ValueError) as e:
        return fail(str(e))

    cc_map = parse_cc_map()

    missing_cc = sorted(set(guests) - cc_map)
    if missing_cc:
        return fail(f"guest(s) {missing_cc} are in {GUESTS_DEF.name} but have "
                    f"no XLATOR_GUEST_CC_<name> in {XSLATOR_MK.name}")

    stale_cc = sorted(cc_map - set(guests))
    if stale_cc:
        named = ", ".join(f"XLATOR_GUEST_CC_{g}" for g in stale_cc)
        return fail(f"{XSLATOR_MK.name} defines {named} for guest(s) the kernel "
                    f"does not register -- either add XLATOR_GUEST rows for "
                    f"them or delete the assignments")

    # e_machine agreement and argv template well-formedness.  A typo in
    # either is invisible until some binary mysteriously refuses to translate,
    # so both are worth a hard failure.
    for name, (machine, tmpl) in sorted(guests.items()):
        em = f"EM_{name.upper()}"
        if em not in elf_machines:
            return fail(f"{GUESTS_DEF.name} guest {name} has no {em} in "
                        f"{ELF_H.name} to check against")
        if elf_machines[em] != machine:
            return fail(f"{GUESTS_DEF.name} says {name} is e_machine {machine}, "
                        f"but {ELF_H.name} says {em} = {elf_machines[em]}")

        why = check_template(name, tmpl)
        if why:
            return fail(f"{GUESTS_DEF.name} guest {name} has argv template "
                        f"{tmpl!r} which {why}; the kernel rejects it at boot "
                        f"and the guest would never be forwarded")

    stray = find_stray_guest_lists()
    if stray:
        return fail("a second guest->e_machine table exists outside "
                    f"{GUESTS_DEF.name}:\n  " + "\n  ".join(stray))

    if GUESTS_DEF.name not in FETCH_PY.read_text():
        return fail(f"{FETCH_PY.name} does not read {GUESTS_DEF.name}; it must "
                    f"not carry its own guest list")

    why = check_bootargs_coverage()
    if why:
        return fail(why)

    arches = parse_xlator_arches()
    print(f"check-xlator-guests: PASS -- {len(guests)} guest(s) "
          f"({', '.join(sorted(guests))}) agree across "
          f"{GUESTS_DEF.name}, {XSLATOR_MK.name} and {ELF_H.name}; "
          f"all {len(arches)} xlator-capable arch(es) "
          f"({', '.join(arches)}) can receive a command line")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
