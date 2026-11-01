"""Validate snapshot analysis against archived guest diagnostics and bad inputs."""
import gzip
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/a20-perf-diff.py'
spec = importlib.util.spec_from_file_location('perf_diff', SCRIPT)
perf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(perf)


class PerfDiff(unittest.TestCase):
    def test_archived_fileio_windows(self):
        archive_path = ROOT / 'docs/status/measurements/fileio-2026-10-22-data.json.gz'
        if not archive_path.exists():
            self.skipTest("archived measurement data is not present in this checkout")
        with gzip.open(archive_path, 'rt') as stream:
            archive = json.load(stream)
        manifest = archive['experiments']['perf-fileio-diagnostic']['manifest']
        samples = [s for s in manifest['samples'] if s['side'] == 'a20']
        self.assertEqual(len(samples), 2)
        expected = [(260, 4720777509), (8, 4777365793)]
        for sample, (count, ticks) in zip(samples, expected):
            result = perf.diff(sample['perf_before'], sample['perf_after'], 1e9)
            fsync = next(r for r in result['syscalls'] if r['nr'] == 82)
            self.assertEqual((fsync['count'], fsync['ticks']), (count, ticks))
            self.assertIn('fsync', fsync['names'])
            # Exercise strict text capture parsing against the actual emitted region.
            pre = sample['pre_output'].split('collection_enabled_before_snapshot: ', 1)[1]
            pre = 'collection_enabled_before_snapshot: ' + pre
            lines = []
            for line in pre.splitlines():
                if not perf.FIELD.fullmatch(line):
                    break
                lines.append(line)
            self.assertEqual(perf.parse_snapshot('\n'.join(lines) + '\n'), sample['perf_before'])

    def test_exact_large_delta_and_new_syscall(self):
        before = {'x': 2**63, perf.STATE: 1}
        after = {'x': 2**63 + 7, perf.STATE: 1, 'syscall_172_count': 2, 'syscall_172_ticks': 13}
        result = perf.diff(before, after)
        self.assertEqual(result['counters']['x'], 7)
        self.assertEqual(result['syscalls'][0]['names'], ['getpid'])
        self.assertNotIn(perf.STATE, result['counters'])

    def test_reject_incomplete_reset_or_inconsistent(self):
        for text in ['x: 1', 'x: 1\nx: 2\n', 'syscall_172_count: 3\n', 'x: -1\n']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                perf.parse_snapshot(text)
        for before, after in [({'x': 5}, {'x': 4}), ({'x': 1}, {'y': 1}),
                              ({'x': 0}, {'x': 1, perf.STATE: 0}),
                              ({'syscall_172_count': 1, 'syscall_172_ticks': 2},
                               {'syscall_172_count': 1, 'syscall_172_ticks': 3})]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                perf.diff(before, after)
        result = subprocess.run([sys.executable, str(SCRIPT), '--ticks-per-second', 'nan'],
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
