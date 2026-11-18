#!/usr/bin/env python3
"""Diff two /proc/a20/perf snapshots or a diagnostic runner manifest.

Ticks measure aggregate elapsed syscall time, including waits, not CPU use.
Capture both snapshots in the same boot and measurement generation. New
windows may be explicitly stopped before the final snapshot.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
STATE = 'collection_enabled_before_snapshot'
FIELD = re.compile(r'([A-Za-z0-9_]+): ([0-9]+)')
SYSCALL = re.compile(r'syscall_([0-9]+)_(count|ticks)')
MAX_U64 = (1 << 64) - 1
METADATA = frozenset((STATE, 'snapshot_schema', 'window_generation', 'window_boot_token',
                      'window_valid', 'window_start_ticks', 'window_stop_ticks',
                      'window_elapsed_ticks', 'ticks_per_second', 'snapshot_frozen',
                      'excluded_syscalls', 'snapshot_complete', 'metric_count', 'syscall_rows'))


def validate(fields):
    if not fields:
        raise ValueError('empty snapshot')
    for name, value in fields.items():
        if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= MAX_U64:
            raise ValueError(f'{name}: expected a uint64 counter')
        match = SYSCALL.fullmatch(name)
        if match:
            other = f'syscall_{match[1]}_' + ('ticks' if match[2] == 'count' else 'count')
            if other not in fields:
                raise ValueError(f'{name}: missing paired field {other}; snapshot may be truncated')
    if STATE in fields and fields[STATE] not in (0, 1):
        raise ValueError('invalid collection state')
    if fields.get('snapshot_schema') == 2:
        missing = METADATA - fields.keys()
        if missing or fields['snapshot_complete'] != 1:
            raise ValueError('incomplete window snapshot: missing metadata or completion marker')
        actual_metrics = sum(name not in METADATA and not SYSCALL.fullmatch(name) for name in fields)
        actual_syscalls = sum(bool(re.fullmatch(r'syscall_[0-9]+_count', name)) for name in fields)
        if actual_metrics != fields['metric_count'] or actual_syscalls != fields['syscall_rows']:
            raise ValueError('snapshot record count mismatch; data may be omitted')
        if fields['ticks_per_second'] == 0:
            raise ValueError('snapshot has no valid clock frequency')
        if fields['window_valid'] and (not fields['window_generation'] or not fields['window_boot_token']):
            raise ValueError('valid window has no generation or boot identity')
    return fields


def parse_snapshot(text):
    if not text.endswith('\n'):
        raise ValueError('snapshot lacks final newline; possible truncation')
    fields = {}
    for line in text.splitlines():
        match = FIELD.fullmatch(line)
        if not match:
            raise ValueError(f'invalid snapshot line: {line[:100]!r}')
        name, value = match.groups()
        if name in fields:
            raise ValueError(f'duplicate field: {name}')
        fields[name] = int(value)
    return validate(fields)


def syscall_names():
    names = {}
    for name, number in re.findall(r'^#define SYS_(\w+)\s+(\d+)\s*$',
                                  (ROOT / 'kernel/include/core/syscall_nr.h').read_text(), re.M):
        names.setdefault(int(number), []).append(name)
    return names


def diff(before, after, ticks_per_second=None):
    validate(before)
    validate(after)
    notes = ['Ticks are aggregate elapsed syscall time, including waits; they are not CPU time.',
             'Snapshots are read sequentially and may include collection commands or concurrent tasks.',
             'Whole omitted trailing records cannot be detected without an external expected record count.']
    if STATE not in before or STATE not in after:
        notes.append('Old snapshot has no collection-state header; collection continuity is unknown.')
    schema = before.get('snapshot_schema')
    if schema == 2 or after.get('snapshot_schema') == 2:
        if schema != 2 or after.get('snapshot_schema') != 2:
            raise ValueError('cannot mix legacy and window snapshots')
        for field in ('window_generation', 'window_boot_token', 'window_start_ticks', 'ticks_per_second'):
            if before[field] != after[field]:
                raise ValueError(f'measurement identity changed: {field}')
        if not before['window_valid'] or not after['window_valid']:
            raise ValueError('measurement window has not started')
        notes[2] = 'Schema counts and completion markers checked; frozen final snapshot recommended.'
        frequency = after['ticks_per_second']
        if ticks_per_second is not None and ticks_per_second != frequency:
            raise ValueError('requested clock frequency disagrees with snapshot')
        ticks_per_second = frequency
        if after.get(STATE) == 0 and not after['snapshot_frozen']:
            raise ValueError('ending snapshot is not a frozen window')
        if not after['snapshot_frozen']:
            notes.append('Ending snapshot is live; stop collection for a stable final measurement.')
    elif after.get(STATE) == 0:
        raise ValueError('collection was disabled before the ending snapshot')
    counters = {}
    for name in before.keys() | after.keys():
        if name in METADATA:
            continue
        if name not in before or name not in after:
            if not SYSCALL.fullmatch(name):
                raise ValueError(f'counter schema changed: {name}')
        delta = after.get(name, 0) - before.get(name, 0)
        if delta < 0:
            raise ValueError(f'{name}: counter decreased ({delta}); reset, wrap, or different boot')
        counters[name] = delta
    names = syscall_names()
    rows = []
    for key, count in counters.items():
        match = SYSCALL.fullmatch(key)
        if not match or match[2] != 'count':
            continue
        nr = int(match[1])
        ticks = counters[f'syscall_{nr}_ticks']
        if count == 0 and ticks:
            raise ValueError(f'syscall {nr}: ticks changed without completed calls; inconsistent snapshots')
        if count == ticks == 0:
            continue
        row = dict(nr=nr, names=names.get(nr, []), count=count, ticks=ticks,
                   ticks_per_call=ticks / count if count else None)
        if ticks_per_second:
            row.update(elapsed_seconds=ticks / ticks_per_second,
                       microseconds_per_call=ticks * 1e6 / ticks_per_second / count if count else None)
        rows.append(row)
    rows.sort(key=lambda row: (-row['ticks'], row['nr']))
    return dict(collection_before=before.get(STATE), collection_after=after.get(STATE),
                ticks_per_second=ticks_per_second, notes=notes, syscalls=rows,
                window=({key: after[key] for key in METADATA} if schema == 2 else None),
                counters=dict(sorted(counters.items())))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('snapshots', nargs='*', type=Path, metavar='SNAPSHOT')
    ap.add_argument('--manifest', type=Path, help='analyze-apk-bottlenecks.py diagnostic manifest')
    ap.add_argument('--case', help='exact manifest case name')
    ap.add_argument('--run', type=int, default=1)
    ap.add_argument('--ticks-per-second', type=float, help='explicit timer frequency; no implicit architecture assumption')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--json', action='store_true', help='all syscall rows and exact integer counter deltas')
    args = ap.parse_args()
    if args.top < 1 or (args.ticks_per_second is not None and
                        (not math.isfinite(args.ticks_per_second) or args.ticks_per_second <= 0)):
        ap.error('--top and --ticks-per-second must be positive finite values')
    try:
        if args.manifest:
            if args.snapshots or not args.case:
                ap.error('--manifest requires --case and no snapshot paths')
            manifest = json.loads(args.manifest.read_text())
            samples = [s for s in manifest['samples'] if s['side'] == 'a20' and
                       s['case'] == args.case and s['run'] == args.run]
            if len(samples) != 1:
                raise ValueError('expected exactly one matching guest sample')
            sample = samples[0]
            if sample['exit_code'] != 0:
                raise ValueError('selected workload failed')
            before, after = sample['perf_before'], sample['perf_after']
        else:
            if len(args.snapshots) != 2 or args.case:
                ap.error('provide BEFORE AFTER, or --manifest PATH --case NAME')
            before, after = [parse_snapshot(path.read_text()) for path in args.snapshots]
        result = diff(before, after, args.ticks_per_second)
    except (ValueError, KeyError, OSError) as error:
        ap.exit(1, f'perf-diff: {error}\n')
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for note in result['notes']:
            print(note, file=sys.stderr)
        print('nr  syscall                     calls          ticks    ticks/call' +
              ('     elapsed_s      us/call' if args.ticks_per_second else ''))
        for row in result['syscalls'][:args.top]:
            name = '/'.join(row['names']) or '(unknown)'
            print(f"{row['nr']:3} {name:26} {row['count']:10} {row['ticks']:14} {row['ticks_per_call']:13.2f}" +
                  (f" {row['elapsed_seconds']:13.6f} {row['microseconds_per_call']:12.3f}"
                   if args.ticks_per_second else ''))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
