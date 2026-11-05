#!/usr/bin/env python3
"""Production Native thread admission with deterministic hosted failure adapters."""
import hashlib
import json
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parent.parent.parent
out = root / '.kernel-build/cluster/native-thread-admission'
out.mkdir(parents=True, exist_ok=True)
report = out / 'report.json'
report.write_text(json.dumps(dict(passed=False)) + '\n')
sources = ['kernel/abi/native/sys_thread_create.c', 'tools/tests/test_native_thread_admission.c']
shim = 'tools/tests/native_thread_shim.h'
cases = []
for arch, compiler, runner in [('x86_64', 'gcc', []),
                              ('aarch64', 'aarch64-linux-gnu-gcc', ['qemu-aarch64']),
                              ('riscv64', 'riscv64-linux-gnu-gcc', ['qemu-riscv64'])]:
    binary = out / arch
    command = [compiler, '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-O2', '-static',
               '-Ikernel/include', '-Ikernel/abi/native', '-include', shim,
               *sources, '-o', str(binary)]
    subprocess.run(command, cwd=root, check=True)
    result = subprocess.run([*runner, str(binary)], cwd=root, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (out / (arch+'.log')).write_text(result.stdout)
    assert result.returncode == 0 and 'PASS Native thread admission:' in result.stdout
    cases.append(dict(arch=arch, passed=True, command=command, runner=runner,
                      binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
    print('PASS Native thread admission:', arch, flush=True)
report.write_text(json.dumps(dict(passed=True, cases=cases,
    evidence='production syscall policy, mocked task/handle/usercopy dependencies; not kernel boot',
    sources={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in sources+[shim, 'kernel/abi/native/sys_validate.h', 'user/liba20rt/a20_types.h']}), indent=2)+'\n')
