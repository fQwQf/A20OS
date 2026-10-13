#!/usr/bin/env python3
"""Unit tests for prepared Native QEMU inputs and their non-overwrite checks."""
import importlib.util
import json
from argparse import Namespace
from pathlib import Path
import tempfile
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / "check-native-sync.py"
SPEC = importlib.util.spec_from_file_location("check_native_sync", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PreparedSnapshotTests(unittest.TestCase):
    def make_snapshot(self, root):
        root.mkdir()
        names = ["kernel.elf", "base.img", "native-futex", "proc/task_object.o", "build.log"]
        (root / "proc").mkdir()
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("fixture:" + name).encode())
        (root / "tracked.patch").write_bytes(b"sealed source patch")
        (root / "untracked-inputs").mkdir()
        (root / "untracked-inputs/task_object.rs").write_bytes(b"sealed untracked rust input")
        hashes = {name: MODULE.sha(root / name) for name in names}
        manifest = {"schema": 1, "arch": "x86_64", "cpus": 2,
                    "task_objects_impl": "rust",
                    "source_inputs_sha256": {"fixture": "source-hash"},
                    "source_tree_state": {"head": "test"},
                    "sealed_source_artifacts_sha256": {
                        "tracked.patch": MODULE.sha(root / "tracked.patch"),
                        "untracked-inputs/task_object.rs": MODULE.sha(
                            root / "untracked-inputs/task_object.rs")},
                    "artifacts_sha256": hashes,
                    "qemu_argv": ["qemu-system-x86_64", "-drive", "file=x,format=raw",
                                  "-kernel", "kernel.elf"]}
        (root / "manifest.json").write_text(json.dumps(manifest))
        return manifest

    def test_valid_snapshot_loads(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "prepared"
            self.make_snapshot(root)
            manifest = MODULE.load_snapshot(root, "x86_64", "rust", 2)
            self.assertEqual(manifest["schema"], 1)

    def test_changed_artifact_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "prepared"
            self.make_snapshot(root)
            (root / "kernel.elf").write_bytes(b"mutated")
            with self.assertRaisesRegex(RuntimeError, "artifact hash mismatch"):
                MODULE.load_snapshot(root, "x86_64", "rust", 2)

    def test_sealed_source_tampering_and_wrong_variant_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "prepared"
            self.make_snapshot(root)
            (root / "tracked.patch").write_bytes(b"mutated patch")
            with self.assertRaisesRegex(RuntimeError, "source evidence hash mismatch"):
                MODULE.load_snapshot(root, "x86_64", "rust", 2)
            with self.assertRaisesRegex(RuntimeError, "configuration"):
                MODULE.load_snapshot(root, "x86_64", "c", 2)

    def test_isolated_make_flags_and_fixture_path(self):
        args = Namespace(cpus=2, task_objects_impl="rust",
                         build_dir=Path("/tmp/a20-kernel"),
                         user_build_dir=Path("/tmp/a20-user"))
        self.assertEqual(MODULE.make_build_flags("aarch64", args), [
            "ARCH=aarch64", "ABI=both", "BRINGUP=0", "NR_CPUS=2",
            "TASK_OBJECTS_IMPL=rust", "BUILD_DIR=/tmp/a20-kernel",
            "USER_BUILD_DIR=/tmp/a20-user", "ALLOW_UNVERIFIED_SMP=1"])
        self.assertEqual(MODULE.native_app_path("aarch64", args),
                         Path("/tmp/a20-user/native-futex-aarch64"))

    def test_default_build_flags_preserve_existing_layout(self):
        args = Namespace(cpus=1, task_objects_impl="c",
                         build_dir=None, user_build_dir=None)
        self.assertEqual(MODULE.make_build_flags("x86_64", args), [
            "ARCH=x86_64", "ABI=both", "BRINGUP=0", "NR_CPUS=1",
            "TASK_OBJECTS_IMPL=c"])
        self.assertEqual(MODULE.native_app_path("x86_64", args),
                         MODULE.ROOT / "user/build/x86_64/native-futex-x86_64")


if __name__ == "__main__":
    unittest.main()
