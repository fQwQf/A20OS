#!/usr/bin/env python3
"""Generate the RV64 mlibc Meson cross file for LLVM/compiler-rt."""
import os
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
prefix, suffix = os.environ.get('LLVM_PREFIX', ''), os.environ.get('LLVM_SUFFIX', '')
clang = os.environ.get('CLANG', prefix+'clang'+suffix)
common = ['--target=riscv64-linux-gnu', '--ld-path='+prefix+'ld.lld'+suffix]
flags = ['-march=rv64gc', '-mabi=lp64d', '-mcmodel=medany', '-fno-stack-protector', '-D_GNU_SOURCE']
rt = str(Path(os.environ.get('LLVM_RT_ROOT', root/'build/toolchains/compiler-rt'))/'riscv64-user/builtins.a')
lines = ['[binaries]', 'c = '+repr([clang, *common]), 'cpp = '+repr([clang, '--driver-mode=g++', *common])]
for name in ['ar', 'strip', 'objcopy']:
    lines.append(name+' = '+repr(prefix+'llvm-'+name+suffix))
lines += ['', '[host_machine]', "system = 'a20'", "cpu_family = 'riscv64'", "cpu = 'riscv64'", "endian = 'little'", '', '[built-in options]', 'c_args = '+repr(flags), 'cpp_args = '+repr(flags), 'c_link_args = '+repr(['-nostdlib', rt]), 'cpp_link_args = '+repr(['-nostdlib', rt])]
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('\n'.join(lines)+'\n')
