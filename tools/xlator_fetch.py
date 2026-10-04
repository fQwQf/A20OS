#!/usr/bin/env python3
"""Fetch a user-space foreign-architecture translator for the build host.

The translator is a plain Linux-ABI program: the kernel has no in-tree
implementation of one and does not want one (see docs/hybrid-kernel/07-*).
So it is obtained the same way the distro world image obtains its
packages -- from Alpine -- and dropped into the FAT32 build directory,
where tools/img.py build_fat32 picks it up and it lands at /bin/ inside
the guest.

Why a download and not a vendored binary:

  * The tree has no precedent for committing a prebuilt third-party ELF,
    and tools/mkrootfs.py:check_overlay_elf_arch exists precisely to keep
    one out of the package path.  Reusing the Alpine CDN keeps provenance
    (a signed-ish index, a version, a checksum) instead of hiding it in a
    binary blob with no recorded origin.
  * A 3.5 MB riscv64 binary that only one optional feature needs does not
    belong in every checkout.

The extraction is content-addressed and idempotent: the target is only
rewritten when the archive it came from differs, so repeated builds are
no-ops.
"""

import argparse
import hashlib
import io
import os
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Alpine release the world's packages are resolved against; keep in step
# with tools/mkrootfs.py's default branch.
ALPINE_BRANCH = "v3.23"

# The one guest registry: kernel/proc/xlator_guests.def.  This file used to
# carry its own GUEST_TARGETS dict, which is how the two drifted -- a guest
# could be added to one and not the other and both would still "work"
# locally.  The kernel includes the .def directly, so parsing it here is
# what keeps `--guest` in step with what the kernel will actually translate.
GUESTS_DEF = REPO / "kernel" / "proc" / "xlator_guests.def"

_GUEST_RE = re.compile(
    r"^\s*XLATOR_GUEST\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)\s*,", re.M)


def load_guest_targets():
    """Guest name -> ELF e_machine, read out of the kernel's own table.

    Only the first two columns matter here; column 3 is the translator's
    argv[0] convention and the kernel owns it.  The comment block is
    skipped by construction -- the regex only matches XLATOR_GUEST lines.
    """
    try:
        text = GUESTS_DEF.read_text()
    except OSError as e:
        die(f"cannot read {GUESTS_DEF}: {e}")
    targets = {m.group(1): int(m.group(2)) for m in _GUEST_RE.finditer(text)}
    if not targets:
        die(f"{GUESTS_DEF} lists no guests; refusing to guess")
    return targets


# Resolved once at import: argparse's choices= comes from here, so a guest
# the kernel does not know cannot even be asked for.
GUESTS = load_guest_targets()

# ELF e_machine of the *host* architectures a translator may be built for.
# This is the check that matters: the kernel re-execs the translator as an
# ordinary native image, so a translator for another architecture would be
# foreign too and would recurse into the hook it is supposed to serve.
HOST_MACHINES = {
    "riscv64": 243,
    "aarch64": 183,
    "x86_64": 62,
    "loongarch64": 258,
    "ppc64le": 21,
}

# Alpine publishes the linux-user emulators as qemu-<guest>, one package
# per guest, usable from any host of the same architecture.
PACKAGE_TEMPLATES = ["qemu-{guest}"]


def die(msg):
    print(f"xlator_fetch: {msg}", file=sys.stderr)
    sys.exit(1)


def must(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        die(f"command failed: {' '.join(cmd)}\n{r.stderr.strip()}")
    return r.stdout


def apk_url(host_arch, filename):
    return (f"https://dl-cdn.alpinelinux.org/alpine/{ALPINE_BRANCH}"
            f"/community/{host_arch}/{filename}")


def resolve_version(host_arch, pkgname):
    """Find <pkgname>'s current version in the branch's APKINDEX.

    Resolving instead of hardcoding a version means the fetch follows the
    branch, and it fails loudly with the real package list if the package
    is ever dropped -- rather than 404ing on a guessed filename.
    """
    url = (f"https://dl-cdn.alpinelinux.org/alpine/{ALPINE_BRANCH}"
           f"/community/{host_arch}/APKINDEX.tar.gz")
    raw = fetch(url)
    proc = subprocess.run(["tar", "-xzO", "APKINDEX"], input=raw,
                          capture_output=True)
    if proc.returncode != 0:
        die(f"cannot read APKINDEX: {proc.stderr.decode(errors='replace')}")

    # APKINDEX is one record per package: blank-line separated, P: before V:.
    version = None
    current = False
    for line in proc.stdout.decode(errors="replace").splitlines():
        if line.startswith("P:"):
            current = line[2:] == pkgname
            if current:
                version = None
        elif line.startswith("V:") and current:
            version = line[2:].strip()
    if not version:
        die(f"{pkgname} is not in {ALPINE_BRANCH}/community/{host_arch}")
    return version


def fetch(url):
    try:
        with urllib.request.urlopen(url, timeout=120) as r:
            return r.read()
    except Exception as e:            # noqa: BLE001 - report whatever the network said
        die(f"cannot fetch {url}: {e}")


def extract_member(blob, member_suffix, dest):
    """Pull one regular file out of the apk's data.tar.gz stream."""
    proc = subprocess.run(
        ["tar", "-xzOf", "-", member_suffix],
        input=blob, capture_output=True,
    )
    if proc.returncode != 0 or not proc.stdout:
        die(f"apk does not contain {member_suffix!r}: "
            f"{proc.stderr.decode(errors='replace').strip()}")
    dest.write_bytes(proc.stdout)


def already_current(dest, version):
    """True when dest was extracted from exactly this package version.

    Skips a repeat download on an unchanged build.  The marker is a
    dotfile so tools/img.py build_fat32 -- which copies only non-dot
    files into the image -- does not ship it into the guest.
    """
    marker = dest.parent / f".{dest.name}.apkver"
    if not (dest.exists() and marker.exists()):
        return False
    # The marker holds "<version> sha256:<digest>"; only the version
    # decides whether a re-download is needed.
    return marker.read_text().split()[0] == version


def verify_guest_elf(path, guest):
    """Assert @path really is a loadable ELF for @guest.

    Guards the cross-build: a probe compiled with the host toolchain would
    be a native image wearing a foreign name, and it would then satisfy a
    translation test by running natively -- a pass that proves nothing.
    """
    raw = Path(path).read_bytes()
    if len(raw) < 20 or raw[:4] != b"\x7fELF":
        die(f"{path} is not an ELF file")
    if raw[4] != 2:
        die(f"{path} is not ELFCLASS64; only 64-bit guests are supported")
    machine = int.from_bytes(raw[18:20], "little")
    want = GUESTS[guest]
    if machine != want:
        die(f"{path} has e_machine={machine}, expected {want} for a "
            f"{guest} guest -- the cross compiler probably produced a "
            f"native binary (check XLATOR_GUEST_CC_{guest} in "
            f"tools/targets-xlator.mk)")
    etype = int.from_bytes(raw[16:18], "little")
    if etype not in (2, 3):
        die(f"{path} has e_type={etype}, expected ET_EXEC or ET_DYN")
    print(f"xlator_fetch: {path} verified as a {guest} (e_machine {machine}) ELF")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check-guest", action="store_true",
                    help="verify --path is an ELF for --guest and exit")
    ap.add_argument("--path", help="file to verify with --check-guest")
    ap.add_argument("--host-arch",
                    help="build host architecture, e.g. riscv64")
    ap.add_argument("--guest", required=True, choices=sorted(GUESTS))
    ap.add_argument("--dest-dir",
                    help="FAT32 staging directory (user/build/<variant>)")
    args = ap.parse_args()

    if args.check_guest:
        if not args.path:
            die("--check-guest requires --path")
        verify_guest_elf(args.path, args.guest)
        return

    if not args.host_arch or not args.dest_dir:
        die("--host-arch and --dest-dir are required without --check-guest")

    dest_dir = Path(args.dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / f"qemu-{args.guest}"

    pkgname = PACKAGE_TEMPLATES[0].format(guest=args.guest)
    version = resolve_version(args.host_arch, pkgname)

    if already_current(dest, version):
        print(f"xlator_fetch: {dest} already at {pkgname}-{version}, skipping")
        return

    url = apk_url(args.host_arch, f"{pkgname}-{version}.apk")
    blob = fetch(url)
    print(f"xlator_fetch: {url} ({len(blob)} bytes)")

    # The apk's payload is a concatenation of gzip streams; probe it with
    # tar so we never shell out to a tar that cannot see stream 2+.
    probe = subprocess.run(["tar", "-tzf", "-"], input=blob, capture_output=True)
    if probe.returncode != 0:
        # Multi-stream apk: concatenate the member streams and retry.
        blob = b"".join(untar_streams(blob))
        probe = subprocess.run(["tar", "-tzf", "-"], input=blob, capture_output=True)
        if probe.returncode != 0:
            die(f"cannot list apk payload: {probe.stderr.decode(errors='replace')}")

    listing = probe.stdout.decode(errors="replace").splitlines()
    member = next((m for m in listing
                   if m.endswith(f"/qemu-{args.guest}")), None)
    if not member:
        die(f"apk payload has no qemu-{args.guest}; members: {listing[:20]}")

    extract_member(blob, member, dest)
    digest = hashlib.sha256(dest.read_bytes()).hexdigest()

    # The translator must be a native image for the build host.  Anything
    # else could not be loaded by execve's normal path, and if it were
    # another foreign architecture the hook would hand it straight back to
    # a translator -- which is the recursion this whole feature is shaped
    # to avoid.
    verify_native_elf(dest, HOST_MACHINES[args.host_arch])

    os.chmod(dest, 0o755)
    (dest_dir / f".{dest.name}.apkver").write_text(
        f"{version} sha256:{digest}\n")
    print(f"xlator_fetch: wrote {dest} ({pkgname}-{version}, sha256 "
          f"{digest[:16]}…, native e_machine {HOST_MACHINES[args.host_arch]}, "
          f"runs e_machine {GUESTS[args.guest]} guests)")


def untar_streams(blob):
    """Yield the decompressed bytes of each concatenated gzip stream."""
    import gzip
    buf = io.BytesIO(blob)
    while True:
        pos = buf.tell()
        d = gzip.GzipFile(fileobj=buf)
        try:
            data = d.read()
        except Exception:              # noqa: BLE001 - not a gzip stream (or truncated)
            buf.seek(pos)
            return
        if not data:
            buf.seek(pos)
            return
        yield data


def verify_native_elf(path, want_machine):
    raw = path.read_bytes()
    if len(raw) < 20 or raw[:4] != b"\x7fELF":
        die(f"{path} is not an ELF file")
    if raw[4] != 2:
        die(f"{path} is not ELFCLASS64")
    machine = int.from_bytes(raw[18:20], "little")
    if machine != want_machine:
        die(f"{path} has e_machine={machine}, expected {want_machine} "
            f"(a translator must be native to the build host, or execve "
            f"cannot load it and the hook would recurse)")
    # e_type must be EXEC or DYN; a relocatable object is not loadable.
    etype = int.from_bytes(raw[16:18], "little")
    if etype not in (2, 3):
        die(f"{path} has e_type={etype}, expected ET_EXEC or ET_DYN")


if __name__ == "__main__":
    main()