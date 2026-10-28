#!/usr/bin/env python3
"""Exercise the production netlink control plane with stock Alpine ip tools."""
import argparse
import importlib.util
from pathlib import Path
import json
import subprocess

spec = importlib.util.spec_from_file_location("bench", Path(__file__).with_name("bench-net-kvm.py"))
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

SCRIPT = '''#!/bin/sh
set -eux
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
ip route add 198.51.100.0/24 via 10.0.2.2 dev en2 table 100 metric 7
ip route show table 100 > /tmp/routes
cat /tmp/routes
grep -F '198.51.100.0/24' /tmp/routes
grep -F '10.0.2.2' /tmp/routes
ip rule add priority 100 from 10.0.2.15/32 table 100
ip rule show > /tmp/rules
cat /tmp/rules
grep -F '10.0.2.15' /tmp/rules
ip route get 198.51.100.5 from 10.0.2.15 > /tmp/lookup
cat /tmp/lookup
grep -F '10.0.2.2' /tmp/lookup
ip rule del priority 100 from 10.0.2.15/32 table 100
ip route del 198.51.100.0/24 via 10.0.2.2 dev en2 table 100
ip route show table 100 > /tmp/routes
if grep -F '198.51.100.0/24' /tmp/routes; then exit 1; fi
echo NET_CONTROL_KVM_PASS
sleep 600
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    base = json.loads(args.baseline.read_text())
    argv = list(next(b['argv'] for b in base['boots'] if b['side'] == 'a20-kvm'))
    image = out / 'world.img'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', base['inputs']['world']['path'], str(image)], check=True)
    argv[argv.index('-kernel') + 1] = str(args.kernel.resolve())
    for i, value in enumerate(argv):
        if 'id=xworld' in value:
            argv[i] = f'file={image},if=none,format=raw,id=xworld'
    script = out / 'control.sh'
    script.write_text(SCRIPT)
    bench.install(image, script, '/etc/a20-benchmark.sh')
    (out / 'manifest.json').write_text(json.dumps(dict(argv=argv, kernel_sha256=bench.apk.sha(args.kernel), script_sha256=bench.apk.sha(script)), indent=2))
    guest = bench.Guest(argv, out / 'serial.log')
    try:
        guest.until(rb'(?:^|\n)NET_CONTROL_KVM_PASS\r*\n', 30)
        print('NET_CONTROL_KVM_PASS', flush=True)
    finally:
        guest.close()
    image.unlink()


if __name__ == '__main__':
    main()
