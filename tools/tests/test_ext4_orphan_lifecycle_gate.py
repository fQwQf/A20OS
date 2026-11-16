"""The lifecycle gate requires real program lines, SMP and clean shutdown."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ext4_orphan_lifecycle_gate", ROOT / "tools/fs/ext4_orphan_lifecycle_gate.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class OrphanLifecycleOracle(unittest.TestCase):
    def test_repeated_results_and_failure_rejection(self):
        text = "[SMP] 2/2 configured CPUs online\n" + "FS_ORPHAN: PASS old_ino=19 bytes=24576\n" * 3 + "[SD] POWER_OFF entry\n"
        self.assertEqual(probe.validate(text, 3), [19])
        for bad in (text.replace("old_ino=19", "old_ino=0"),
                    text.replace("bytes=24576", "bytes=0"),
                    text.replace("2/2 configured", "1/2 configured"),
                    text.replace("FS_ORPHAN: PASS", "# echo FS_ORPHAN: PASS"),
                    text.replace("[SD] POWER_OFF entry", ""),
                    text + "FS_ORPHAN: FAIL orphan data\n", text + "[SD] audit errors=1\n"):
            with self.assertRaises(ValueError):
                probe.validate(bad, 3)
        with self.assertRaises(ValueError):
            probe.validate(text, 4)


if __name__ == "__main__":
    unittest.main()
