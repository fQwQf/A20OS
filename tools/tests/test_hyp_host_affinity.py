"""Fixed-load HYP acceptance never guesses vCPU identity from thread names."""
import importlib.util
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('hyp_lifecycle_probe',ROOT/'tools/probes/hyp_lifecycle_probe.py')
probe=importlib.util.module_from_spec(spec);spec.loader.exec_module(probe)

class HostAffinity(unittest.TestCase):
 def test_qmp_ids_separate_two_vcpus_from_same_named_io_threads(self):
  records=[{'cpu-index':0,'thread-id':103},{'cpu-index':1,'thread-id':105}]
  self.assertEqual(probe.affinity_layout(records,[101,102,103,104,105],[10,12,14]),
                   {101:14,102:14,103:10,104:14,105:12})
 def test_missing_duplicate_and_nonexistent_ids_fail_before_assignment(self):
  for records in ([{'cpu-index':0,'thread-id':103}],
   [{'cpu-index':0,'thread-id':103},{'cpu-index':1,'thread-id':103}],
   [{'cpu-index':0,'thread-id':103},{'cpu-index':1,'thread-id':999}]):
   with self.subTest(records=records),self.assertRaises(RuntimeError):
    probe.affinity_layout(records,[101,103,105],[10,12,14])
if __name__=='__main__':unittest.main()
