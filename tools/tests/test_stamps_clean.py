import argparse
import importlib.util
import tempfile
import unittest
from pathlib import Path


STAMPS = Path(__file__).resolve().parents[1] / "stamps.py"
spec = importlib.util.spec_from_file_location("a20_stamps", STAMPS)
stamps = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(stamps)


class CleanSymlinkTests(unittest.TestCase):
    def test_clean_empties_build_target_and_preserves_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            (target / "riscv64-qemu-virt-riscv64-both-dev-preempt").mkdir(parents=True)
            (target / "riscv64-qemu-virt-riscv64-both-dev-preempt" / "kernel.o").write_text("x")
            link = repo / ".kernel-build"
            link.symlink_to(target, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertTrue(link.is_symlink())
            self.assertEqual(list(target.iterdir()), [])

    def test_unexpected_target_content_is_left_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            target.mkdir()
            marker = target / "keep-me"
            marker.write_text("important")
            link = repo / ".kernel-build"
            link.symlink_to(target, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                with self.assertRaises(SystemExit):
                    stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertEqual(marker.read_text(), "important")
            self.assertTrue(link.is_symlink())


if __name__ == "__main__":
    unittest.main()
