#!/usr/bin/env python3
"""Build pinned upstream BusyBox with the project's existing musl toolchain."""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import urllib.request

from prepare_busybox import validate_elf, write_changed

VERSION = "1.37.0"
SHA256 = "3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4"
ROOT = Path(__file__).resolve().parents[2]


def source_tree(cache: Path) -> Path:
    archive = cache / f"busybox-{VERSION}.tar.bz2"
    cache.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        with urllib.request.urlopen(f"https://busybox.net/downloads/{archive.name}", timeout=60) as response:
            data = response.read(8 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise ValueError("BusyBox source checksum mismatch")
        write_changed(archive, data, 0o644)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError(f"BusyBox source cache checksum mismatch: {archive}")
    source = cache / "source" / f"busybox-{VERSION}"
    if not (source / "Makefile").exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as package:
            package.extractall(source.parent, filter="data")
    return source


def build(args) -> None:
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    config = {key: getattr(args, key) for key in ("arch", "nommu", "cc", "cflags", "ldflags", "crt_start", "crt_end", "libc")}
    identity = hashlib.sha256((json.dumps(config, sort_keys=True) + SHA256 + Path(__file__).read_text()).encode()).hexdigest()[:20]
    out = cache / "source-builds" / f"{args.arch}-{identity}"
    with (cache / ".source.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        source = source_tree(cache)
    out.mkdir(parents=True, exist_ok=True)
    with (out / ".build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        binary = out / "busybox_unstripped"
        if not binary.is_file():
            # Keep make's dependency generation and feature probes in upstream.
            # The compiler adapter supplies the same CRT/libc as local A20 tools.
            write_changed(out / "compiler.json", json.dumps(config).encode(), 0o644)
            wrapper = '''#!/usr/bin/env python3
import json, pathlib, shlex, subprocess, sys
c=json.loads(pathlib.Path(__file__).with_name("compiler.json").read_text())
a=sys.argv[1:]
probe=any(x.startswith(("-dump", "-print", "--version")) for x in a)
compile_only=probe or any(x in a for x in ("-c", "-E", "-S", "-M", "-MM"))
cmd=shlex.split(c["cc"])+[x for x in shlex.split(c["cflags"]) if x not in ("-MMD", "-MP")]
if any(x in a for x in ("-r", "-Wl,-r")):
    cmd += ["-nostdlib", "-no-pie"] + a
elif compile_only:
    cmd+=a
else:
    if c["nommu"]:
        a=[x for x in a if x not in ("-static", "-pie", "-Wl,-pie")]
    crt=shlex.split(c["crt_start"])
    cmd+=shlex.split(c["ldflags"])+["-L"+str(pathlib.Path(crt[0]).parent)]+crt+a+shlex.split(c["crt_end"])+shlex.split(c["libc"])
sys.exit(subprocess.call(cmd))
'''
            write_changed(out / "a20-cc", wrapper.encode())
            env = os.environ.copy()
            for key in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "ARCH", "CROSS_COMPILE", "KCONFIG_ALLCONFIG"):
                env.pop(key, None)
            target_arch = {"arm32": "arm", "riscv32": "riscv", "riscv64": "riscv"}.get(args.arch, args.arch)
            command = ["make", "-C", str(source), f"O={out}", f"ARCH={target_arch}", f"CC={out / 'a20-cc'}", "HOSTCC=cc"]
            subprocess.run(command + ["defconfig"], env=env, check=True)
            changes = {"STATIC": "y", "PIE": "y" if args.nommu else "n", "NOMMU": "y" if args.nommu else "n",
                # This standalone userland has no OpenSSL executable. Avoid
                # probing a missing helper via vfork: upstream 1.37 calls exit
                # on exec failure, poisoning the parent's musl exit lock.
                "WERROR": "n", "FEATURE_WGET_HTTPS": "y", "FEATURE_WGET_OPENSSL": "n",
                "FEATURE_SH_EMBEDDED_SCRIPTS": "n", "USE_BB_CRYPT": "y", "USE_BB_CRYPT_SHA": "y",
                "FEATURE_MOUNT_NFS": "n", "FEATURE_INETD_RPC": "n", "SEEDRNG": "n", "TC": "n",
                "SHA1_HWACCEL": "n", "SHA256_HWACCEL": "n"}
            # RV32 is time64-only: upstream hwclock unconditionally calls the
            # absent legacy settimeofday syscall for kernel timezone handling.
            # It is outside our staged applet list; keep other ISAs unchanged.
            if args.arch == "riscv32":
                changes["HWCLOCK"] = "n"
            lines = (out / ".config").read_text().splitlines()
            for key, value in changes.items():
                lines = [line for line in lines if not line.startswith(f"CONFIG_{key}=") and line != f"# CONFIG_{key} is not set"]
                lines.append(f"CONFIG_{key}={value}" if value != "n" else f"# CONFIG_{key} is not set")
            (out / ".config").write_text("\n".join(lines) + "\n")
            subprocess.run(command + ["oldconfig"], input=b"\n" * 2048, env=env, check=True)
            # The host strip tool may not understand the target ISA. Keep the
            # upstream linked ELF; no host binutils or target execution needed.
            subprocess.run(command + [f"-j{args.jobs}", "busybox_unstripped", "busybox.links"], env=env, check=True)
        data = binary.read_bytes()
        validate_elf(data, args.arch)
        write_changed(args.output, data)
        write_changed(args.output.with_name(".busybox.links"), (out / "busybox.links").read_bytes(), 0o644)
        write_changed(args.output.with_name(".busybox.config"), (out / ".config").read_bytes(), 0o644)
        write_changed(args.output.with_name(".busybox.source.json"), json.dumps({"version": VERSION, "sha256": SHA256, "config": config}, indent=2).encode(), 0o644)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--nommu", type=int, default=0)
    parser.add_argument("--cache", type=Path, default=ROOT / "build/cache/busybox")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    for key in ("cc", "cflags", "ldflags", "crt-start", "crt-end", "libc"):
        parser.add_argument("--" + key, required=True)
    args = parser.parse_args()
    try:
        build(args)
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"busybox build: {error}\n")


if __name__ == "__main__":
    main()
