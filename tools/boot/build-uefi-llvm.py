#!/usr/bin/env python3
"""Build a native PE/COFF UEFI application using Clang and lld-link."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch', choices=['x86_64', 'aarch64'], required=True)
    p.add_argument('--kernel-bin', type=Path, required=True)
    p.add_argument('--kernel-elf', type=Path, required=True)
    p.add_argument('--load-address', required=True)
    p.add_argument('--log', default='1')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    prefix, suffix = os.environ.get('LLVM_PREFIX', ''), os.environ.get('LLVM_SUFFIX', '')
    clang = os.environ.get('CLANG', prefix+'clang'+suffix)
    memory = subprocess.check_output([sys.executable, str(ROOT/'tools/elf_load_span.py'), str(a.kernel_elf), '--expected-base', a.load_address], text=True).strip()
    a.out.parent.mkdir(parents=True, exist_ok=True)
    stage = a.out.parent / ('llvm-uefi-'+a.arch)
    stage.mkdir(exist_ok=True)
    blob = stage / 'kernel.S'
    blob.write_text('.section .kernel,"dr"\n.balign 16\n.global _binary_kernel_bin_start\n_binary_kernel_bin_start:\n.incbin "'+str(a.kernel_bin.resolve())+'"\n.global _binary_kernel_bin_end\n_binary_kernel_bin_end:\n')
    flags = ['--target='+a.arch+'-pc-windows-msvc', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fshort-wchar', '-mno-stack-arg-probe', '-Ikernel/include', '-DKERNEL_MEMORY_BYTES='+memory, '-DA20_LOADER_LOG='+a.log, '-DKERNEL_LOAD_ADDRESS='+a.load_address]
    if a.arch == 'x86_64':
        flags += ['-mno-red-zone', '-DX86_KERNEL_LOAD_ADDRESS='+a.load_address]
    for source, target in [(ROOT/f'kernel/arch/{a.arch}/boot/uefi/{a.arch}_loader.c', stage/'loader.obj'), (blob, stage/'kernel.obj')]:
        subprocess.run([clang, *flags, '-c', str(source), '-o', str(target)], cwd=ROOT, check=True)
    subprocess.run([prefix+'lld-link'+suffix, '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib', '/machine:'+('x64' if a.arch == 'x86_64' else 'arm64'), '/out:'+str(a.out), str(stage/'loader.obj'), str(stage/'kernel.obj')], check=True)

if __name__ == '__main__':
    main()
