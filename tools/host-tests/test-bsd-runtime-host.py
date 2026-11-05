#!/usr/bin/env python3
"""Build and exercise real BSD memory/task-TLS adapters with host fault hooks."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clang', required=True, type=Path, help='compiler with -femulated-tls')
parser.add_argument('--out', required=True, type=Path)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
common = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fno-builtin', '-no-pie',
          '-I.', '-Ikernel/arch/x86_64/include', '-Ikernel/include', '-Ikernel/external/f-stack/lib']
if args.sanitize:
    common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
port = 'kernel/net/backends/bsd/runtime/'
fixtures = 'tools/tests/bsd_runtime_host/'
rows = []

def run(name, argv):
    result = subprocess.run(argv, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out/(name+'.log')).write_bytes(result.stdout)
    rows.append(dict(name=name, argv=[str(a) for a in argv], rc=result.returncode))
    (out/'manifest.json').write_text(json.dumps(dict(sanitize=args.sanitize, stages=rows), indent=2))
    if result.returncode:
        raise SystemExit(f'{name} failed: {out/(name+".log")}')

run('memory-build', ['cc', *common, '-DBSD_MEMORY_LIMIT=65536', port+'memory.c',
                    fixtures+'hooks.c', 'tools/tests/bsd_memory_host.c', '-pthread', '-o', str(out/'memory')])
run('memory', [str(out/'memory')])
run('tls-compile', [str(args.clang.resolve()), '-femulated-tls', '-fno-pie', '-c',
                    fixtures+'tls_values.c', '-o', str(out/'tls-values.o')])
run('tls-build', ['cc', *common, port+'memory.c', port+'tls.c', fixtures+'hooks.c',
                  fixtures+'tls_main.c', str(out/'tls-values.o'), '-pthread', '-o', str(out/'tls')])
run('tls', [str(out/'tls')])
run('explicit-tls-build', ['cc', *common, '-DA20_BSD_TEST_EXPLICIT_TLS=1',
                          port+'memory.c', port+'tls.c', fixtures+'hooks.c',
                          fixtures+'tls_main.c', fixtures+'tls_values.c',
                          '-pthread', '-o', str(out/'explicit-tls')])
run('explicit-tls', [str(out/'explicit-tls')])
sources = [port+'memory.c', port+'memory.h', port+'tls.c', port+'tls.h',
           'kernel/net/backends/bsd/include/a20_bsd_explicit_tls.h',
           fixtures+'hooks.c', fixtures+'hooks.h', fixtures+'tls_values.c', fixtures+'tls_main.c',
           'tools/tests/bsd_memory_host.c', 'tools/host-tests/test-bsd-runtime-host.py']
(out/'inputs.json').write_text(json.dumps({p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources}, indent=2))
print('BSD_RUNTIME_HOST_PASS', out)
