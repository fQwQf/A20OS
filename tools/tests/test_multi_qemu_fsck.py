#!/usr/bin/env python3
"""Prevent successful process status from hiding ext4 consistency failures."""
import importlib.util
from pathlib import Path
import unittest

script = Path(__file__).resolve().parents[1] / 'check-multi-qemu-kernel.py'
spec = importlib.util.spec_from_file_location('multi_qemu', script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FsckAcceptance(unittest.TestCase):
    image = Path('/tmp/scratch-ext4.img')
    clean = ('e2fsck 1.47.2 (1-Jan-2025)\n' +
             ''.join(f'Pass {i}: Checking filesystem\n' for i in range(1, 6)) +
             '/tmp/scratch-ext4.img: 13/8192 files (0.0% non-contiguous), 2596/32768 blocks\n')

    def test_clean(self):
        module.verify_fsck_output(self.clean, self.image)

    def test_zero_exit_diagnostics(self):
        for diagnostic in ('Group descriptor 0 checksum is 0x3874, should be 0x9659.  IGNORED.',
                           'Free blocks count wrong (30236, counted=30172).',
                           'Free inodes count wrong (8180, counted=8179).',
                           'Fix? no', 'An unfamiliar diagnostic'):
            with self.subTest(diagnostic=diagnostic), self.assertRaises(RuntimeError):
                module.verify_fsck_output(diagnostic + '\n' + self.clean, self.image)

    def test_incomplete_or_other_image(self):
        for text in ('', self.clean.replace('Pass 3: Checking filesystem\n', ''),
                     self.clean.replace('/tmp/scratch-ext4.img:', '/tmp/different.img:')):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                module.verify_fsck_output(text, self.image)


if __name__ == '__main__':
    unittest.main()
