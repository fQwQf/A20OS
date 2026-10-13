#!/usr/bin/env python3
"""Run production flat user memory ownership with hosted lock/allocator adapters.

No physical board or kernel-boot claim. Native SVC/futex integration is covered
by check-mcu-program and check-mcu-program-rtl.
"""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / '.kernel-build/cluster/flat-user'
out.mkdir(parents=True, exist_ok=True)
sources = ['kernel/mm/flat_user.c', 'tools/tests/test_flat_user.c']
shim = 'tools/tests/flat_user_shim.h'
results = []
for arch, compiler, runner in [('x86_64', 'gcc', []),
                               ('aarch64', 'aarch64-linux-gnu-gcc', ['qemu-aarch64']),
                               ('riscv64', 'riscv64-linux-gnu-gcc', ['qemu-riscv64']),
                               ('x86_64-sanitized', 'gcc', [])]:
    binary = out / (arch + '-flat-test')
    flags = ['-O2', '-static'] if arch != 'x86_64-sanitized' else [
        '-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
    build = [compiler, '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
             '-DA20_FLAT_HOST_TEST', '-Ikernel/include', '-include', shim,
             *sources, '-o', str(binary)]
    subprocess.run(build, cwd=root, check=True)
    run = subprocess.run([*runner, str(binary)], cwd=root, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    log = out / (arch + '.log')
    log.write_text(run.stdout)
    assert run.returncode == 0 and 'PASS flat user:' in run.stdout, str(log)
    results.append(dict(arch=arch, passed=True, build=build, runner=runner,
                        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
    print('PASS flat ownership:', arch, flush=True)
(out / 'report.json').write_text(json.dumps(dict(passed=True, tests=results,
    evidence='hosted dependencies, production ownership implementation; not a board boot',
    sources={s: hashlib.sha256((root/s).read_bytes()).hexdigest() for s in
             sources + [shim, 'kernel/include/mm/flat_user.h']}), indent=2) + '\n')
