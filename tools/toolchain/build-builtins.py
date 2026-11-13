#!/usr/bin/env python3
"""Build pinned LLVM compiler-rt builtins without a GNU compiler or binutils."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
VERSION = '19.1.7'
SOURCES = {
    'compiler-rt': 'c12b6e764202c615c1a3af9a13d477846878757ae0e29e5f8979215a6958fffc',
    'cmake': '11c5a28f90053b0c43d0dec3d0ad579347fc277199c005206b963c19aae514e3',
}
TARGETS = {
    'riscv64': ('riscv64-unknown-elf', '-march=rv64gc -mabi=lp64d -mcmodel=medany'),
    'riscv32': ('riscv32-unknown-elf', '-march=rv32imafdc -mabi=ilp32d -mcmodel=medany'),
    'x86_64': ('x86_64-unknown-none-elf', '-mno-red-zone'),
    'aarch64': ('aarch64-none-elf', '-mno-outline-atomics'),
    'loongarch64': ('loongarch64-unknown-elf', '-march=loongarch64 -mabi=lp64d'),
    'arm32': ('arm-none-eabihf', '-march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard'),
    'armv7m': ('arm-none-eabi', '-mcpu=cortex-m3 -mthumb -mfloat-abi=soft'),
    'ppc64le': ('powerpc64le-none-elf', '-mcpu=power8 -mabi=elfv2'),
}

def tool(name):
    name = os.environ.get('LLVM_PREFIX', '') + name + os.environ.get('LLVM_SUFFIX', '')
    path = shutil.which(name)
    if not path:
        raise SystemExit(f'missing LLVM tool: {name}')
    return str(Path(path).absolute())

def source_tree():
    base = ROOT / 'build/toolchains/sources'
    base.mkdir(parents=True, exist_ok=True)
    with (base / '.fetch.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for name, digest in SOURCES.items():
            filename = f'{name}-{VERSION}.src.tar.xz'
            archive = base / filename
            if not archive.exists():
                temporary = archive.with_suffix('.download')
                urllib.request.urlretrieve(f'https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/{filename}', temporary)
                temporary.replace(archive)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
                raise SystemExit(f'SHA256 mismatch: {archive}')
            directory = base / f'{name}-{VERSION}.src'
            if not directory.exists():
                with tarfile.open(archive) as tar:
                    tar.extractall(base, filter='data')
        link = base / 'cmake'
        if not link.exists():
            link.symlink_to(f'cmake-{VERSION}.src', target_is_directory=True)
    return base / f'compiler-rt-{VERSION}.src/lib/builtins'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch', choices=TARGETS, required=True)
    p.add_argument('--kind', choices=['kernel', 'user', 'mcu'], default='user')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=4)
    a = p.parse_args()
    clang = shlex.split((os.environ.get('CLANG') or tool('clang')))
    if len(clang) != 1:
        p.error('CLANG must name one executable; use LLVM_PREFIX for its directory')
    clang = shutil.which(clang[0]) or clang[0]
    triple, flags = TARGETS[a.arch]
    if a.arch == 'riscv64' and a.kind == 'kernel':
        flags = flags.replace('-mabi=lp64d', '-mabi=lp64')
    if a.kind == 'mcu':
        if a.arch != 'riscv32':
            p.error('mcu runtime is only for riscv32')
        flags = '-march=rv32imc_zicsr_zifencei -mabi=ilp32 -mcmodel=medany'
    flags += (' -fPIC' if a.kind == 'user' else ' -fno-pic -fno-pie')
    flags += ' -ffreestanding -fno-stack-protector -nostdlibinc -isystem ' + str(ROOT / 'tools/toolchain/include')
    output = a.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.with_suffix('.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        identity = {'version': VERSION, 'build_script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    'assert_header': hashlib.sha256((ROOT / 'tools/toolchain/include/assert.h').read_bytes()).hexdigest(), 'clang': str(Path(clang).resolve()),
                    'compiler': subprocess.check_output([clang, '--version'], text=True),
                    'target': triple, 'flags': flags, 'ar': tool('llvm-ar')}
        stamp = output.with_suffix('.json')
        if output.exists() and stamp.exists() and json.loads(stamp.read_text()) == identity:
            return
        source = source_tree()
        build = output.parent / 'cmake'
        if build.exists():
            shutil.rmtree(build)
        cmd = ['cmake', '-S', str(source), '-B', str(build), '-G', 'Unix Makefiles',
               '-DCMAKE_C_COMPILER='+clang, '-DCMAKE_ASM_COMPILER='+clang,
               '-DCMAKE_C_COMPILER_TARGET='+triple, '-DCMAKE_ASM_COMPILER_TARGET='+triple,
               '-DCMAKE_C_FLAGS='+flags, '-DCMAKE_ASM_FLAGS='+flags,
               '-DCMAKE_AR='+tool('llvm-ar'), '-DCMAKE_RANLIB='+tool('llvm-ranlib'),
               '-DCMAKE_NM='+tool('llvm-nm'), '-DCMAKE_LINKER='+tool('ld.lld'),
               '-DCMAKE_SYSTEM_NAME=Generic', '-DCMAKE_BUILD_TYPE=Release',
               '-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON', '-DCOMPILER_RT_BAREMETAL_BUILD=ON',
               '-DCOMPILER_RT_BUILTINS_ENABLE_PIC='+('ON' if a.kind == 'user' else 'OFF'), '-DCOMPILER_RT_INCLUDE_TESTS=OFF', '-DLLVM_RUNTIMES_BUILD=ON']
        subprocess.run(cmd, check=True)
        subprocess.run(['cmake', '--build', str(build), '-j', str(a.jobs)], check=True)
        archives = list(build.rglob('libclang_rt.builtins*.a'))
        if len(archives) != 1:
            raise SystemExit(f'expected one builtins archive, found {archives}')
        shutil.copyfile(archives[0], output)
        stamp.write_text(json.dumps(identity, indent=2)+'\n')

if __name__ == '__main__':
    main()
