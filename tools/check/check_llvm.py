#!/usr/bin/env python3
"""Build and boot LLVM bring-up kernels, retaining per-architecture logs."""
import argparse
import json
import os
import re
import shutil
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def forbidden_tools(trace):
    forbidden = set()
    for line in trace.splitlines():
        match = re.search(r'execve\("([^"]+)"', line)
        if not match:
            continue
        name = Path(match[1]).name
        if name.startswith(('llvm-', 'rust-', 'clang-')):
            continue
        if re.fullmatch(r'(?:[\w.]+-)*(?:gcc|g\+\+|cpp|cc|c\+\+|cc1|cc1plus|collect2|as|ld|ld.bfd|ld.gold|ar|ranlib|nm|objcopy|objdump|readelf|strip|size)(?:-\d+)?', name):
            forbidden.add(match[1])
    return forbidden

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arches', nargs='+', default=['riscv64', 'aarch64', 'x86_64', 'loongarch64', 'arm32', 'riscv32'])
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--timeout', type=int, default=60)
    p.add_argument('--build-only', action='store_true')
    p.add_argument('--audit', action='store_true', help='trace subprocesses and reject GNU compiler/binutils executions')
    p.add_argument('--strace', default='strace')
    a = p.parse_args()
    out = ROOT / 'build/llvm-validation'
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    env = dict(os.environ, CCACHE_DISABLE='1') if a.audit else os.environ
    if a.audit and not shutil.which(a.strace):
        p.error('--audit requires strace (or --strace=/path/to/strace)')
    for arch in a.arches:
        flags = ['TOOLCHAIN=llvm', f'ARCH={arch}', 'BRINGUP=1', 'NET_STACK=lwip']
        with (out / f'{arch}.log').open('w') as log:
            cmd = ['make', *flags, f'-j{a.jobs}', 'kernel-only']
            trace = out / f'{arch}-exec.log'
            if a.audit:
                cmd = [a.strace, '-f', '-e', 'trace=execve', '-s', '8192', '-o', str(trace), *cmd]
            rc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        result = {'build': rc == 0}
        if a.audit:
            forbidden = forbidden_tools(trace.read_text())
            result['gnu_tools'] = sorted(forbidden)
            if forbidden:
                result['build'] = False
        if result['build'] and not a.build_only:
            raw = subprocess.check_output(['make', '-s', '--no-print-directory', *flags, '_qemu_argv'], cwd=ROOT, text=True)
            argv = shlex.split(raw.strip().splitlines()[-1])
            with (out / f'{arch}-boot.log').open('w') as log:
                try:
                    rc = subprocess.run(argv, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=a.timeout).returncode
                except subprocess.TimeoutExpired:
                    rc = 124
            text = (out / f'{arch}-boot.log').read_text(errors='replace')
            result.update(boot=rc == 0 and 'part ok' in text and 'System is going down for power-off NOW' in text, boot_exit=rc)
        results[arch] = result
        print(arch, result, flush=True)
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return int(any(not r['build'] or r.get('boot') is False for r in results.values()))

if __name__ == '__main__':
    raise SystemExit(main())
