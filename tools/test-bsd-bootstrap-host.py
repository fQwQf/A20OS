#!/usr/bin/env python3
"""Execute the isolated BSD SYSINIT/UMA image on a host allocator/clock substrate.

This is a bootstrap test, not an A20OS backend or Internet connectivity test.
No BSD initialization, socket or protocol function is replaced by a host stub.
"""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--core', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
core = args.core.resolve()
previous = json.loads((core/'manifest.json').read_text())
if not previous['emulated_tls'] or not previous['partial_link'].get('no_native_tls_checked'):
    parser.error('the BSD core must be checked for emulated TLS before execution')
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
obj = core/'bsd-core-isolated.o'
manifest = dict(core_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(), stages=[])
(out/'core-manifest.json').write_text(json.dumps(previous, indent=2))
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

def run(name, argv, timeout=30):
    row = dict(name=name, argv=[str(a) for a in argv])
    try:
        result = subprocess.run(argv, cwd=root, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
        row['rc'] = result.returncode
        output = result.stdout
    except subprocess.TimeoutExpired as error:
        row['error'] = 'timeout'
        output = error.stdout or b''
    (out/(name+'.log')).write_bytes(output)
    manifest['stages'].append(row)
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    if row.get('rc') != 0:
        raise SystemExit(f'{name} failed; see {out/(name+".log")}')
    return output

# Reuse the recorded BSD compiler flags; do not compile the probe as Linux C.
bsd = next(row['argv'] for row in previous['sources'] if row['owner'] == 'upstream')
fixture = 'tools/tests/bsd_bootstrap_host/loopback_bsd.c'
run('loopback-compile', [*bsd[:bsd.index('-c')], '-c', fixture, '-o', str(out/'loopback.o')])
run('loopback-isolate', ['objcopy', '--prefix-symbols=a20_bsd_', str(out/'loopback.o')])
run('loopback-host-imports', ['objcopy', '--redefine-syms=' + str(core/'host-imports.map'), str(out/'loopback.o')])
run('link', ['cc', '-std=gnu11', '-g', '-no-pie', '-pthread',
             'tools/tests/bsd_runtime_host/hooks.c', 'tools/tests/bsd_bootstrap_host/main.c',
             str(obj), str(out/'loopback.o'), '-Wl,-T,kernel/net/backends/bsd/sets.ld', '-o', str(out/'bootstrap')])
# A freestanding combined object for the kernel bootstrap diagnostic: no host
# hooks, libc or Linux clock/socket implementation is linked into this file.
run('kernel-object', ['ld', '-r', str(obj), str(out/'loopback.o'), '-o', str(out/'kernel-bootstrap.o')])
manifest['kernel_object_sha256'] = hashlib.sha256((out/'kernel-bootstrap.o').read_bytes()).hexdigest()
output = run('boot', [str(out/'bootstrap')], timeout=15)
if not all(marker in output for marker in [b'BSD_BOOTSTRAP_HOST_PASS', b'BSD_UDP_LOOPBACK_PASS', b'BSD_TCP_LOOPBACK_PASS']):
    raise SystemExit('missing actual initialization marker')
print('BSD_BOOTSTRAP_HOST_PASS', out)
