#!/usr/bin/env python3
"""Repeat complete paired guest lifecycles without restarting the named host.

Use hyp-host-riscv64-acceptance to enable the supervisor's 5 ms timer audit.
Both guests pause/resume, execute commands and shut down on every round.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/cli'))
from a20_runtime import _path, _qmp


def affinity_layout(cpu_records, thread_ids, cpus):
    """Use authoritative QMP IDs even when all Linux thread names are equal."""
    if len(cpus) != 3 or len(set(cpus)) != 3:
        raise RuntimeError('--host-cpus requires three distinct CPUs: vCPU0,vCPU1,IO')
    cpu_threads = {int(row['thread-id']): int(row['cpu-index']) for row in cpu_records}
    if len(cpu_threads) != 2 or set(cpu_threads.values()) != {0, 1} or not set(cpu_threads) <= set(thread_ids):
        raise RuntimeError('QMP must identify both live host vCPU thread IDs')
    return {tid: cpus[cpu_threads.get(tid, 2)] for tid in thread_ids}


def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    host_dir = _path(args.host)
    console = host_dir / 'console.log'
    argv_path = host_dir / 'argv.json'
    result = dict(passed=False, host=args.host, profile=args.profile,
                  requested_lifecycles=args.cycles, completed_lifecycles=0, rounds=[])
    started = time.monotonic()
    try:
        argv = json.loads(argv_path.read_text())
        (args.output / 'host-argv.json').write_text(json.dumps(argv, indent=2) + '\n')
        if isinstance(argv, list) and '-kernel' in argv:
            kernel = Path(argv[argv.index('-kernel') + 1])
            if not kernel.is_absolute(): kernel = ROOT / kernel
            result['kernel_sha256'] = hashlib.sha256(kernel.read_bytes()).hexdigest()
        pid = int((host_dir / 'qemu.pid').read_text())
        threads = sorted(Path(f'/proc/{pid}/task').iterdir(), key=lambda p: int(p.name))
        cpu_records = _qmp(host_dir, 'query-cpus-fast')
        result['qmp_cpu_threads'] = cpu_records
        if args.host_cpus:
            cpus = [int(value) for value in args.host_cpus.split(',')]
            if not set(cpus) <= os.sched_getaffinity(0):
                raise RuntimeError('--host-cpus contains an unavailable CPU')
            layout = affinity_layout(cpu_records, [int(t.name) for t in threads], cpus)
            for tid, cpu in layout.items():
                os.sched_setaffinity(tid, {cpu})
        result['host_thread_affinity'] = [dict(tid=int(thread.name),
            name=(thread / 'comm').read_text().strip(), cpus=sorted(os.sched_getaffinity(int(thread.name))))
            for thread in threads]
        result['base_images_sha256'] = {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(host_dir.glob('base-*.qcow2'))
        }
        for index in range(1, args.cycles // 2 + 1):
            report = args.output / f'round-{index:03d}.json'
            begin = time.monotonic()
            print(f'HYP_LIFECYCLE: round {index}/{args.cycles // 2}', flush=True)
            subprocess.run([sys.executable, str(ROOT / 'tools/probes/hyp_pair_probe.py'),
                            '--host', args.host, '--profile', args.profile,
                            '--timeout', str(args.timeout), '--report', str(report)],
                           cwd=ROOT, check=True)
            pair = json.loads(report.read_text())
            if pair['status'] != 'pass' or pair.get('completed_lifecycles') != 2:
                raise RuntimeError('pair did not complete two full lifecycles')
            if pair['baseline_stats'][:4] != pair['final_stats'][:4]:
                raise RuntimeError('pair resource counts did not return to baseline')
            if result['rounds'] and result['rounds'][0]['baseline_stats'][:4] != pair['final_stats'][:4]:
                raise RuntimeError('resource counts drifted across rounds')
            result['rounds'].append(dict(round=index, elapsed_seconds=round(time.monotonic()-begin, 3),
                baseline_stats=pair['baseline_stats'], final_stats=pair['final_stats'],
                rpc_max_ms=pair['rpc_max_ms']))
            result['completed_lifecycles'] += 2
            text = console.read_text(errors='replace')
            if re.search(r'VMD_TIMER_AUDIT:.*FAIL|PANIC|Kernel panic|Kernel BUG|LOCK-STALL', text):
                raise RuntimeError('host timer audit or kernel failed')
            (args.output / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
        audit = re.findall(r'VMD_TIMER_AUDIT: samples=(\d+) max_ms=(\d+) bound_ms=150 PASS', text)
        if not audit:
            raise RuntimeError('missing opt-in supervisor timer audit samples')
        result['timer_samples'], result['timer_max_ms'] = map(int, audit[-1])
        result['passed'] = result['completed_lifecycles'] == args.cycles
    except Exception as error:
        result['error'] = str(error)
        print('HYP_LIFECYCLE: FAIL ' + str(error), file=sys.stderr)
    finally:
        result['elapsed_seconds'] = round(time.monotonic() - started, 3)
        if console.exists(): (args.output / 'host-console.log').write_bytes(console.read_bytes())
        (args.output / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    if result['passed']:
        print(f'HYP_LIFECYCLE: PASS cycles={args.cycles} timer_max_ms={result["timer_max_ms"]}')
    return 0 if result['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--profile', default='a20-ramfs-riscv64-smp2')
    parser.add_argument('--host-cpus', help='optional fixed Linux CPU affinity: vCPU0,vCPU1,IO')
    parser.add_argument('--cycles', type=int, default=100)
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.cycles < 2 or args.cycles % 2 or args.timeout <= 0:
        parser.error('cycles must be a positive even number, timeout must be positive')
    return run(args)


if __name__ == '__main__':
    raise SystemExit(main())
