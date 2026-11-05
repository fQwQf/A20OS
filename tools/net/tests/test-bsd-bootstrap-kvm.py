#!/usr/bin/env python3
"""Boot the BSD runtime/loopback diagnostic in the real A20OS kernel under KVM.

A pass proves kernel execution of BSD SYSINIT/UMA and TCP/UDP loopback, not
physical-interface networking, Linux socket ABI integration or Internet access.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cpus', default='4,6')
args = parser.parse_args()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
kernel = args.kernel.resolve()
argv = ['taskset', '-c', args.cpus, 'qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm',
        '-cpu', 'host', '-smp', '2', '-m', '2G', '-display', 'none', '-monitor', 'none',
        '-serial', 'stdio', '-no-reboot', '-kernel', str(kernel), '-net', 'none']
manifest = dict(kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(), argv=argv, passed=False)
p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
data = bytearray()
try:
    deadline = time.monotonic() + 60
    with (out/'serial.log').open('wb') as log:
        while time.monotonic() < deadline:
            if select.select([p.stdout], [], [], 1)[0]:
                chunk = os.read(p.stdout.fileno(), 65536)
                if not chunk: break
                data.extend(chunk); log.write(chunk); log.flush()
                marker = data.find(b'BSD_BOOTSTRAP_KVM_PASS')
                if marker >= 0 and b'\n' in data[marker:]:
                    manifest['passed'] = all(marker in data for marker in
                        [b'BSD_BOOTSTRAP_KVM_BEGIN', b'BSD_UDP_LOOPBACK_PASS', b'BSD_TCP_LOOPBACK_PASS'])
                    break
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait()
    manifest['qemu_exit_after_cleanup'] = p.returncode
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
if not manifest['passed']:
    raise SystemExit(f'BSD_KVM_BOOTSTRAP_FAILED: {out/"serial.log"}')
print('BSD_KVM_BOOTSTRAP_PASS', out)
