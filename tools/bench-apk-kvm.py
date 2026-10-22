#!/usr/bin/env python3
"""Compare identical APK workloads in A20OS and Linux guests on the same KVM setup.

Uses private copies of the benchmark ext4 image, never the host root filesystem.
A Linux kernel/initramfs with virtio-blk and ext4 support must be supplied.
"""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('apk_bench', ROOT / 'tools/bench-apk-linux.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

class KVMGuest(bench.Guest):
    def until(self, pattern, timeout):
        # Linux serial termios may emit CRCRLF; preserve raw logs and accept it.
        return super().until(pattern.replace(rb'\r?\n', rb'\r*\n'), timeout)


LINUX_INIT = '''#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root SHELL=/bin/sh TERM=vt100
/bin/busybox mkdir -p /proc /sys /dev /run /tmp
/bin/busybox mount -t proc proc /proc || true
/bin/busybox mount -t sysfs sysfs /sys || true
/bin/busybox mount -t devtmpfs devtmpfs /dev || true
/bin/busybox mkdir -p /dev/shm /tmp /run
/bin/busybox mount -t tmpfs -o mode=1777 tmpfs /dev/shm || true
echo A20_BENCH_READY
/bin/sh /etc/a20-benchmark.sh
while :; do /bin/busybox sleep 3600; done
'''


def install(image, src, dst):
    subprocess.run(['/usr/sbin/debugfs', '-w', '-R', f'write {src} {dst}', str(image)],
                   check=True, capture_output=True)
    actual = subprocess.check_output(['/usr/sbin/debugfs', '-R', f'cat {dst}', str(image)], stderr=subprocess.DEVNULL)
    if actual != src.read_bytes():
        raise RuntimeError(f'installed file mismatch: {dst}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, required=True, help='bench-apk-linux.py manifest with QEMU/image/APK fingerprints')
    ap.add_argument('--a20-kernel', type=Path, required=True)
    ap.add_argument('--linux-kernel', type=Path, required=True)
    ap.add_argument('--linux-initrd', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--seconds', type=int, default=5)
    ap.add_argument('--timeout', type=int, default=90)
    ap.add_argument('--cases', default='cpu-1,cpu-2,memory-write,threads-2,switch,fork,sha256')
    ap.add_argument('--fileio', action='store_true', help='also validate 64MiB random-write cases; fixed five seconds each')
    args = ap.parse_args()
    if args.runs < 1 or args.seconds < 1 or args.timeout < 1:
        ap.error('runs, seconds and timeout must be positive')
    source = json.loads(args.baseline.read_text())
    chosen = {k: bench.cases(args.seconds)[k] for k in args.cases.split(',')}
    if 'pipe' in chosen:
        ap.error('packet/stream pipe fallback is not a comparable case')
    if args.fileio:
        for freq in [100, 16384]:
            chosen[f'fileio-{freq}'] = (['/bin/sh', '/usr/bin/a20-fileio', '/var/a20-fileio',
                                        '/bin/busybox', str(freq), 'sysbench'], 'MiB/s')
    qemu = list(source['qemu_argv'])
    base = next(Path(a.split('file=', 1)[1].split(',', 1)[0]) for a in qemu if 'id=xworld' in a)
    fat = next(Path(a.split('file=', 1)[1].split(',', 1)[0]) for a in qemu if 'id=x0' in a)
    inputs = [base, fat, args.a20_kernel.resolve(), args.linux_kernel.resolve(), args.linux_initrd.resolve()]
    if args.fileio:
        inputs.append(ROOT / 'tools/bottleneck-sysbench-fileio.sh')
    for path in inputs:
        if not path.is_file():
            ap.error(f'missing input: {path}')
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    init = out / 'linux-init';init.write_text(LINUX_INIT)
    # debugfs write preserves executable permissions from the source.
    init.chmod(0o755)
    manifest = dict(created=time.strftime('%Y-%m-%dT%H:%M:%S%z'), baseline=str(args.baseline.resolve()),
                    qemu_template=qemu, cases=chosen, runs=args.runs, seconds=args.seconds,
                    input_sha256={str(p): bench.sha(p) for p in inputs},
                    binary_sha256=source['binary_sha256'], runner_sha256=bench.sha(__file__),
                    samples=[], boots=[], counter_policy='A20 disable each case and audit; Linux has no A20 counters')
    (out / 'runner.py.txt').write_text(Path(__file__).read_text())
    if args.fileio:
        (out / 'fileio-wrapper.sh.txt').write_text((ROOT / 'tools/bottleneck-sysbench-fileio.sh').read_text())
    def save():
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    save()
    for run in range(1, args.runs + 1):
        for side in (['linux-kvm', 'a20-kvm'] if run % 2 else ['a20-kvm', 'linux-kvm']):
            private = out / f'{run}-{side}.img'
            subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(base), str(private)], check=True)
            lines = ['#!/bin/sh', 'export LC_ALL=C OPENSSL_CONF=/dev/null OPENSSL_ia32cap=0',
                     'uname -a', 'cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || true',
                     'sha256sum ' + ' '.join(manifest['binary_sha256']), 'echo APK_BENCH_INFO_DONE']
            for i, (case, (argv, _)) in enumerate(chosen.items()):
                token = f'{run}_{i}'
                if side == 'a20-kvm':
                    lines.append(bench.COUNTERS_OFF)
                lines += [f'echo APK_BENCH_BEGIN_{token}', shlex.join(argv), f'r=$?; echo APK_BENCH_END_{token} $r']
                if side == 'a20-kvm':
                    lines += bench.counter_audit_script(token)
            lines += ['echo KVM_SUITE_DONE']
            script = out / f'{run}-{side}.sh';script.write_text('\n'.join(lines) + '\n')
            install(private, script, '/etc/a20-benchmark.sh')
            if side == 'linux-kvm':
                install(private, init, '/etc/a20-linux-init')
            if args.fileio:
                install(private, ROOT / 'tools/bottleneck-sysbench-fileio.sh', '/usr/bin/a20-fileio')
            argv = [a.replace(str(base), str(private)) for a in qemu]
            argv[argv.index('-kernel') + 1] = str(args.a20_kernel.resolve() if side == 'a20-kvm' else args.linux_kernel.resolve())
            if side == 'linux-kvm':
                argv += ['-initrd', str(args.linux_initrd.resolve()), '-append',
                         'root=/dev/vdb rw init=/etc/a20-linux-init console=ttyS0 quiet']
            manifest['boots'].append(dict(run=run, side=side, argv=argv, image_sha256=bench.sha(private),
                                           script_sha256=bench.sha(script)))
            save()
            guest = KVMGuest(argv, out / f'{run}-{side}-qemu.log')
            try:
                info, _ = guest.until(rb'(?:^|\n)APK_BENCH_INFO_DONE\r?\n', 20)
                info = info.decode(errors='replace').replace('\r', '');(out / f'{run}-{side}-info.log').write_text(info)
                if any(f'{h}  {p}' not in info for p, h in manifest['binary_sha256'].items()):
                    raise RuntimeError('guest executable/library fingerprint mismatch')
                for i, (case, (case_argv, unit)) in enumerate(chosen.items()):
                    output, rc, wall = guest.result(f'{run}_{i}', args.timeout)
                    output = output.replace('\r', '')
                    row = dict(run=run, side=side, case=case, argv=case_argv, unit=unit,
                               exit_code=rc, wall_seconds=wall, output=output)
                    if side == 'a20-kvm':
                        row.update(bench.read_counter_audit(guest, f'{run}_{i}'))
                    if case.startswith('fileio-'):
                        body = output.split('FILEIO_RUN_BEGIN\n', 1)[-1].split('FILEIO_RUN_END', 1)[0]
                        found = re.search(r'written, MiB/s:\s*([\d.]+)', body)
                        value = float(found[1]) if found and 'FILEIO: PASS verified_bytes=67108864 cleaned=1' in output else None
                    else:
                        value = bench.metric(case, output, args.seconds)
                    row.update(value=value, status='ok' if rc == 0 and value is not None and value > 0 else 'failed')
                    elapsed = re.search(r'total time:\s*([\d.]+)s', output)
                    if elapsed and not case.startswith('fileio-'):
                        row['reported_seconds'] = float(elapsed[1])
                        if abs(wall - float(elapsed[1])) > max(1, args.seconds * 0.25):
                            row.update(status='invalid-clock', value=None)
                    manifest['samples'].append(row)
                    (out / f'{run}-{side}-{case}.log').write_text(output)
                    save();print(f"{run} {side} {case}: {row['status']} {row['value']} {unit}", flush=True)
                guest.until(rb'(?:^|\n)KVM_SUITE_DONE\r?\n', 15)
            finally:
                guest.close()
    manifest['inputs_unchanged'] = all(bench.sha(p) == h for p, h in manifest['input_sha256'].items())
    summary = {}
    for case in chosen:
        entry = {}
        for side in ['linux-kvm', 'a20-kvm']:
            rows = [s for s in manifest['samples'] if s['side'] == side and s['case'] == case]
            values = [s['value'] for s in rows if s['status'] == 'ok']
            entry[side] = dict(values=values, complete=len(values) == args.runs,
                               median=statistics.median(values) if values else None,
                               min=min(values) if values else None, max=max(values) if values else None)
        if all(entry[s]['complete'] for s in ['linux-kvm', 'a20-kvm']):
            entry['a20_over_linux_kvm'] = entry['a20-kvm']['median'] / entry['linux-kvm']['median']
        summary[case] = entry
    manifest['summary'] = summary;save();print(json.dumps(summary, indent=2), flush=True)
    return 0 if manifest['inputs_unchanged'] and all(s['status'] == 'ok' for s in manifest['samples']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
