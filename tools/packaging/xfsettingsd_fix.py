#!/usr/bin/env python3
"""Build/install the source fix for xfsettingsd 4.20.1's bus-disconnect UAF."""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
VERSION = '4.20.1'
URL = f'https://archive.xfce.org/src/xfce/xfce4-settings/4.20/xfce4-settings-{VERSION}.tar.bz2'
# Alpine 3.23-stable community/xfce4-settings/APKBUILD.
SOURCE_SHA512 = 'd29307ab93f4db961154541bddc9cfa80886c7ab7bcd42941995c1b9ff24fd8816e65bd690e35c247e25913c352d653386743663443def0b825d415f40b50ef9'
PATCH = ROOT / 'packages/patches/xfce4-settings/0001-gtk-helper-bus-lifetime.patch'
# Multiarch Alpine image used for the native target toolchain, not host glibc.
IMAGE = 'alpine:3.23@sha256:85fe1e81d6758c208f3e1eed4338a1997e19d4be002d4dd32d3100c9a8c010a0'
PLATFORMS = {'x86_64': 'linux/amd64', 'aarch64': 'linux/arm64',
             'riscv64': 'linux/riscv64', 'loongarch64': 'linux/loong64'}
MACHINES = {'x86_64': 62, 'aarch64': 183, 'riscv64': 243, 'loongarch64': 258}

BUILD = r'''
set -eu
test "$(apk --print-arch)" = "$A20_TARGET_ARCH"
mkdir -p /work/root
cp -a /bin /etc /lib /sbin /usr /var /work/root/
mkdir -p /work/root/dev /work/root/tmp /work/root/root /work/root/build
mknod -m 666 /work/root/dev/null c 1 3
mknod -m 666 /work/root/dev/zero c 1 5
mknod -m 666 /work/root/dev/urandom c 1 9
apk --root /work/root --initdb --no-cache add build-base pkgconf \
    exo-dev garcon-dev intltool libcanberra-dev libnotify-dev libxcursor-dev \
    libxfce4ui-dev libxi-dev libxklavier-dev libxrandr-dev xf86-input-libinput-dev
cp /input/source.tar.bz2 /input/fix.patch /work/root/build/
chroot /work/root /bin/sh -ec '
    cd /build
    tar -xjf source.tar.bz2
    cd xfce4-settings-4.20.1
    patch -p1 < ../fix.patch
    export CFLAGS="-Os -fomit-frame-pointer -fno-plt"
    export LDFLAGS="-Wl,--as-needed,-O1,--sort-common"
    ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --disable-static --enable-libxklavier --enable-xrandr \
        --enable-sound-settings --enable-xorg-libinput --enable-x11 --enable-wayland
    grep -q "#define ENABLE_X11 1" config.h
    grep -q "#define ENABLE_WAYLAND 1" config.h
    grep -q "#define ENABLE_DISPLAY_SETTINGS 1" config.h
    make -j4 -C protocols
    make -j4 -C common
    make -j4 -C xfsettingsd
'
src=/work/root/build/xfce4-settings-4.20.1
cp "$src/xfsettingsd/xfsettingsd" /out/xfsettingsd
cp "$src/COPYING" /out/COPYING
cp "$src/config.h" /out/config.h
chroot /work/root apk info -v > /out/build-packages.txt
chroot /work/root cc --version > /out/compiler.txt
chmod 755 /out/xfsettingsd
chown "$A20_OUTPUT_UID:$A20_OUTPUT_GID" /out/*
'''


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_archive() -> Path:
    dest = ROOT / f'build/downloads/xfce4-settings-{VERSION}.tar.bz2'
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists():
        with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as tmp:
            temporary = Path(tmp.name)
            try:
                with urllib.request.urlopen(URL, timeout=60) as response:
                    shutil.copyfileobj(response, tmp)
                tmp.close()
                if hashlib.sha512(temporary.read_bytes()).hexdigest() != SOURCE_SHA512:
                    raise RuntimeError('xfce4-settings source checksum mismatch')
                temporary.replace(dest)
            finally:
                temporary.unlink(missing_ok=True)
    if hashlib.sha512(dest.read_bytes()).hexdigest() != SOURCE_SHA512:
        raise RuntimeError(f'xfce4-settings source checksum mismatch: {dest}')
    return dest


def build(arch: str, output: Path | None = None) -> Path:
    if arch not in PLATFORMS:
        raise RuntimeError(f'no xfsettingsd source builder for {arch}')
    archive = source_archive()
    output = output or ROOT / f'build/upstream/xfsettingsd/{arch}'
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    key = {'arch': arch, 'version': VERSION, 'source_sha512': SOURCE_SHA512,
           'patch_sha256': digest(PATCH), 'builder_sha256': digest(Path(__file__)),
           'image': IMAGE}
    with (output / 'build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        binary = output / 'xfsettingsd'
        manifest = output / 'manifest.json'
        if binary.exists() and manifest.exists():
            old = json.loads(manifest.read_text())
            if old.get('inputs') == key and old.get('binary_sha256') == digest(binary):
                return binary
        # The compiler/rootfs live on a bounded tmpfs; the disk retains only
        # sources and the small finished artifact. Use the normal host gate.
        sys.path.insert(0, str(ROOT / 'tools/cli'))
        from a20_resource import Policy, Requirement, gate
        policy = Policy.from_env()
        gate(Requirement(3072 + policy.reserve_mem_mb, 4, policy.min_disk_mb),
             policy, ROOT, wait=False, label='xfsettingsd native source build')
        with tempfile.TemporaryDirectory(prefix='.build-', dir=output) as work:
            work = Path(work)
            inputs, products = work / 'input', work / 'output'
            inputs.mkdir()
            products.mkdir()
            shutil.copy2(archive, inputs / 'source.tar.bz2')
            shutil.copy2(PATCH, inputs / 'fix.patch')
            command = ['docker', 'run', '--rm', '--read-only', '--memory=3g', '--cpus=4',
                       '--platform', PLATFORMS[arch], '--tmpfs', '/work:rw,size=2g,exec,dev',
                       '--mount', f'type=bind,src={inputs},dst=/input,readonly',
                       '--mount', f'type=bind,src={products},dst=/out',
                       '-e', f'A20_TARGET_ARCH={arch}',
                       '-e', f'A20_OUTPUT_UID={os.getuid()}',
                       '-e', f'A20_OUTPUT_GID={os.getgid()}', IMAGE, 'sh', '-ec', BUILD]
            print(f'xfsettingsd: building {VERSION} with lifecycle patch for {arch}', flush=True)
            with (output / 'build.log').open('w') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f'xfsettingsd build failed; see {output / "build.log"}')
            data = (products / 'xfsettingsd').read_bytes()
            if data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != MACHINES[arch]:
                raise RuntimeError('xfsettingsd builder emitted the wrong ELF architecture')
            for path in products.iterdir():
                os.replace(path, output / path.name)
            manifest.write_text(json.dumps({'inputs': key, 'binary_sha256': digest(binary),
                'config_sha256': digest(output / 'config.h'),
                'packages_sha256': digest(output / 'build-packages.txt')}, indent=2) + '\n')
            return binary


def install_if_present(root: Path, arch: str, sudo: list[str]) -> None:
    if not (root / 'usr/bin/xfsettingsd').exists():
        return
    database = (root / 'lib/apk/db/installed').read_text()
    versions = [line[2:] for block in database.split('\n\n')
                if 'P:xfce4-settings' in block.splitlines()
                for line in block.splitlines() if line.startswith('V:')]
    if versions != [VERSION + '-r0']:
        raise RuntimeError(f'review xfsettingsd lifecycle patch for installed versions {versions}')
    binary = build(arch)
    source = root / 'usr/share/a20/xfsettingsd-lifetime'
    subprocess.run(sudo + ['mkdir', '-p', str(source)], check=True)
    subprocess.run(sudo + ['cp', str(binary), str(root / 'usr/bin/xfsettingsd')], check=True)
    # Ship corresponding source and the build script with the modified GPL
    # executable; no opaque binary is committed to the repository.
    for path in [source_archive(), PATCH, Path(__file__), binary.parent / 'COPYING',
                 binary.parent / 'manifest.json', binary.parent / 'build-packages.txt']:
        subprocess.run(sudo + ['cp', str(path), str(source / path.name)], check=True)
    subprocess.run(sudo + ['chmod', '0755', str(root / 'usr/bin/xfsettingsd')], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', required=True, choices=PLATFORMS)
    parser.add_argument('--output-dir', type=Path)
    arguments = parser.parse_args()
    print(build(arguments.arch, arguments.output_dir))
