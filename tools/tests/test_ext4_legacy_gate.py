"""Legacy-journal tests discover slots and keep dirty/clean devices distinct."""
import importlib.util
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/fs'))
try:
 spec=importlib.util.spec_from_file_location('ext4_legacy_gate',ROOT/'tools/fs/ext4_legacy_journal_gate.py')
 gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
finally:sys.path.pop(0)
class LegacyDevices(unittest.TestCase):
 def test_reverse_probe_order_uses_observed_capacities(self):
  output=b'[VIRTIO0] Block device ready: capacity=139264 sectors\n[VIRTIO1] Block device ready: capacity=131072 sectors\n[VIRTIO2] Block device ready: capacity=262144 sectors\n'
  self.assertEqual(gate.device_script(output),'ext4_legacy_ro_test /dev/vdb clean\next4_legacy_ro_test /dev/vda dirty\npoweroff\n')
 def test_missing_or_duplicate_capacity_cannot_select_an_unrelated_disk(self):
  with self.assertRaises(gate.GateError):gate.device_script(b'[VIRTIO0] Block device ready: capacity=262144 sectors')
if __name__=='__main__':unittest.main()
