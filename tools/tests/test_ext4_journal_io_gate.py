"""A controlled error power cut needs the kernel's explicit refusal boundary."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/fs'))
try:
 spec=importlib.util.spec_from_file_location('ext4_journal_io_gate',ROOT/'tools/fs/ext4_journal_io_gate.py')
 gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
finally:sys.path.pop(0)
GOOD='\n'.join(('[VIRTIO0] bad completion head=0 status=1',
 '[VIRTIO0] I/O error: status=1 lba=128 (after 3 retries)',
 '[EXT4/JBD2] commit failed: -5 (transaction stays pending)',
 '[SD] poweroff refused: filesystem shutdown failed (-5)'))

class JournalIoGate(unittest.TestCase):
 def invoke(self,text):
  with tempfile.TemporaryDirectory() as d:
   log=Path(d)/'log'
   argv=[sys.executable,'-u','-c','import time;print('+repr(text)+',flush=True);time.sleep(5)']
   result=gate.fault_boot(argv,log,1)
   self.assertEqual(result,log.read_bytes())
   return result
 def test_only_a_retained_failed_transaction_allows_power_cut(self):
  self.assertIn(b'poweroff refused',self.invoke(GOOD))
 def test_missing_retry_commit_refusal_and_bad_completion_fail(self):
  for marker in ('I/O error:','commit failed:','poweroff refused:','bad completion'):
   text='\n'.join(line for line in GOOD.splitlines() if marker not in line)
   with self.subTest(marker=marker),self.assertRaises(RuntimeError):self.invoke(text)
 def test_successful_shutdown_and_panic_cannot_count_as_fault_refusal(self):
  for extra in ('\n[SD] POWER_OFF entry','\n[PANIC] failed','\nUBSAN: bad shift'):
   with self.subTest(extra=extra),self.assertRaises(RuntimeError):self.invoke(GOOD+extra)

if __name__=='__main__':unittest.main()
