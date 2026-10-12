#!/usr/bin/env python3
"""Run identical production execution-info/CW codec contracts on three ISAs.

These are hosted codec tests, not physical boards or kernel syscall tests.
Actual Native syscall coverage is smoke-native-cluster and check-mcu-program.
"""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / '.kernel-build/cluster/execution'
out.mkdir(parents=True, exist_ok=True)
source = root / 'tools/tests/test_clx_work.c'
results = []
for arch, compiler, runner in [
    ('x86_64', 'gcc', []),
    ('aarch64', 'aarch64-linux-gnu-gcc', ['qemu-aarch64']),
    ('riscv64', 'riscv64-linux-gnu-gcc', ['qemu-riscv64']),
    ('x86_64-sanitized', 'gcc', []),
]:
    binary = out / (arch + '-work-test')
    flags = ['-static', '-O2'] if arch != 'x86_64-sanitized' else [
        '-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
    build = [compiler, '-std=gnu99', '-Wall', '-Wextra', '-Werror', *flags,
             '-Ikernel/include', str(source), '-o', str(binary)]
    subprocess.run(build, cwd=root, check=True)
    command = [*runner, str(binary)]
    run = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=60)
    log = out / (arch + '.log')
    log.write_text(run.stdout)
    assert run.returncode == 0 and 'execution contract: 9 profiles' in run.stdout and 'domain contract: 32 capacities' in run.stdout, str(log)
    results.append(dict(arch=arch, build=build, command=command, log=str(log),
                        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                        passed=True, evidence='hosted ISA execution, not kernel boot'))
    print('PASS execution contract:', arch, flush=True)
report = dict(passed=True, tests=results, physical_boards_tested=False,
              source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
              headers_sha256={s: hashlib.sha256((root / s).read_bytes()).hexdigest() for s in [
                  "kernel/include/uapi/a20/execution.h", "kernel/include/uapi/a20/execution_domain.h",
                  "kernel/include/uapi/a20/cluster_work.h"]})
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
