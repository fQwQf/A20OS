import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "musl_build_identity.py"
SPEC = importlib.util.spec_from_file_location("musl_build_identity", MODULE_PATH)
musl_build_identity = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(musl_build_identity)


class MuslBuildIdentityTests(unittest.TestCase):
    def test_generated_build_dirs_do_not_change_source_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "musl"
            source.mkdir()
            (source / "configure").write_text("source-v1\n")
            before, count_before = musl_build_identity.source_digest(source)

            generated = source / "build-riscv64"
            generated.mkdir()
            (generated / "config.mak").write_text("generated\n")
            (source / ".git").mkdir()
            (source / ".git" / "index").write_text("metadata\n")
            after, count_after = musl_build_identity.source_digest(source)

            self.assertEqual(before, after)
            self.assertEqual(count_before, count_after)

    def test_source_file_change_changes_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "musl"
            source.mkdir()
            config = source / "configure"
            config.write_text("source-v1\n")
            first, _ = musl_build_identity.source_digest(source)
            config.write_text("source-v2\n")
            second, _ = musl_build_identity.source_digest(source)
            self.assertNotEqual(first, second)


if __name__ == "__main__":
    unittest.main()
