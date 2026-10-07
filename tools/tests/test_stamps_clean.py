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
            target.mkdir()
            (target / ".a20-build-root").write_text("a20-build-root-v1\n")
            (target / "riscv64-qemu-virt-riscv64-both-dev-preempt").mkdir()
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
            self.assertEqual([p.name for p in target.iterdir()], [".a20-build-root"])

    def test_clean_accepts_loongarch32_build_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            target.mkdir()
            (target / ".a20-build-root").write_text("a20-build-root-v1\n")
            (target / "loongarch32-nailoong-bringup").mkdir(parents=True)
            link = repo / ".kernel-build"
            link.symlink_to(target, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertTrue(link.is_symlink())
            self.assertEqual([p.name for p in target.iterdir()], [".a20-build-root"])

    def test_unexpected_target_content_is_left_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            target.mkdir()
            (target / ".a20-build-root").write_text("a20-build-root-v1\n")
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

    def test_clean_refuses_unmarked_target_even_with_legal_build_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            target = base / "cache"
            repo.mkdir()
            build = target / "riscv64-qemu-virt-dev"
            build.mkdir(parents=True)
            marker = build / "keep-me"
            marker.write_text("important")
            link = repo / ".kernel-build"
            link.symlink_to(target, target_is_directory=True)
            old_repo = stamps.REPO
            stamps.REPO = repo
            try:
                with self.assertRaisesRegex(SystemExit, "unmarked"):
                    stamps.cmd_clean(argparse.Namespace(find_root=[], rm_rf=[".kernel-build"], rm_f=[]))
            finally:
                stamps.REPO = old_repo
            self.assertEqual(marker.read_text(), "important")
            self.assertTrue(link.is_symlink())

    def test_clean_refuses_repository_parent_even_with_build_named_children(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            repo.mkdir()
            (base / ".a20-build-root").write_text("a20-build-root-v1\n")
            build = base / "riscv64-qemu-virt-dev"
            build.mkdir()
            marker = build / "keep-me"
            marker.write_text("important")
            link = repo / ".kernel-build"
            link.symlink_to(base, target_is_directory=True)
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
