"""Crash gate must reject killed/failed guests and filesystem-check failures."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('ext4_journal_gate',ROOT/'tools/fs/ext4_journal_gate.py')
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)


class JournalGate(unittest.TestCase):
    def test_prompt_and_cursor_queries_then_natural_shutdown(self):
        program=r'''
import sys
for expected in ('sync', 'poweroff'):
 print('# ',end='',flush=True)
 print('\x1b[6n',end='',flush=True)
 assert sys.stdin.buffer.read(6)==b'\x1b[1;1R'
 assert input()==expected
 print('executed '+expected,flush=True)
print('[SD] POWER_OFF entry',flush=True)
'''
        with tempfile.TemporaryDirectory() as d:
            log=Path(d)/'guest.log'
            gate.feed([sys.executable,'-u','-c',program],'sync\npoweroff\n',22,log,5)
            self.assertIn('executed sync',log.read_text())
            self.assertIn('executed poweroff',log.read_text())

    def test_timeout_failed_exit_and_missing_shutdown_are_rejected(self):
        for script in ('import time; time.sleep(10)',
                       "print('[SD] POWER_OFF entry');raise SystemExit(1)",
                       "print('A20OS version fake')",
                       "print('UBSAN: bad shift\\n[SD] POWER_OFF entry')"):
            with self.subTest(script=script),tempfile.TemporaryDirectory() as d:
                log=Path(d)/'guest.log'
                with self.assertRaises(gate.GateError):
                    gate.feed([sys.executable,'-u','-c',script],'poweroff\n',0,log,.2)
                self.assertTrue(log.exists())

    def test_e2fsck_exit_code_is_required_even_without_known_error_word(self):
        with patch.object(gate.subprocess,'run',return_value=subprocess.CompletedProcess([],4,'unexpected failure','')):
            with self.assertRaises(gate.GateError):gate.fsck_clean('disk.img','e2fsck')


if __name__ == '__main__':unittest.main()
