#!/usr/bin/env python3
"""Execute production shared device mailboxes on concurrent host CPU shims."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--arch', choices=['x86_64', 'aarch64', 'riscv64'], default='x86_64')
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
sources = ['kernel/core/shared_device_call.c', 'kernel/core/shared_mmio.c', 'tools/shared-device-test/main.c']
compiler = {'x86_64': 'gcc', 'aarch64': 'aarch64-linux-gnu-gcc', 'riscv64': 'riscv64-linux-gnu-gcc'}[args.arch]
runner = [] if args.arch == 'x86_64' else ['qemu-' + args.arch]
checks = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'] if not runner else ['-static']
command = [compiler, '-std=gnu11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
           *checks,
           '-DCONFIG_SHARED_DEVICE_CALL', '-DCONFIG_NR_CPUS=4',
           '-Itools/shared-device-test', '-Ikernel/include', *sources, '-o', str(out/'test')]
report = {'passed': False, 'arch': args.arch, 'command': command, 'runner': runner,
          'sanitizers': ['ASan', 'UBSan'] if not runner else [],
          'scope': 'production mailboxes, four concurrent host CPU shims; not real IRQ or device proof'}
try:
    subprocess.run(command, cwd=root, check=True, capture_output=True, text=True)
    run = subprocess.run([*runner, str(out/'test')], check=True, capture_output=True, text=True, timeout=90)
    (out/'run.log').write_text(run.stdout + run.stderr)
    if 'PASS shared device calls=80000 endpoints=4 nested=316' not in run.stdout:
        raise RuntimeError('missing acceptance output')
    if 'PASS shared MMIO pairs=80000 widths=1,2,4,8' not in run.stdout:
        raise RuntimeError('missing MMIO acceptance output')
    report['passed'] = True
    print(run.stdout, end='')
except Exception as error:
    report['error'] = repr(error)
    (out/'failure.log').write_text(str(getattr(error, 'stdout', '')) + str(getattr(error, 'stderr', '')))
    raise
finally:
    report['sources'] = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [root/sources[0], root/sources[1], root/'kernel/include/core/shared_mmio.h', root/'kernel/include/core/shared_device_call.h',
                  *sorted((root/'tools/shared-device-test').rglob('*.c')),
                  *sorted((root/'tools/shared-device-test').rglob('*.h'))]}
    (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
