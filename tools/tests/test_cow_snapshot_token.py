"""Check real C lock-token adapters independently of the Rust transaction."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SnapshotToken(unittest.TestCase):
    def test_lock_and_reference_identity(self):
        text = (ROOT / "kernel/mm/cow.c").read_text()
        struct = text[text.index("struct a20_cow_shared_snapshot {"):text.index("uint64_t a20_cow_policy_flags(")]
        functions = text[text.index("int a20_cow_share_snapshot("):text.index("int a20_cow_requires_private_copy(")]
        release = text[text.index("void a20_cow_release_ref("):text.index("int mm_fork_clone_page(")]
        fixture = (ROOT / "tools/tests/cow_snapshot_token_host.c").read_text()
        fixture = fixture.replace("/* SNAPSHOT_STRUCT */", struct).replace("/* SNAPSHOT_FUNCTIONS */", functions).replace("/* RELEASE_FUNCTION */", release)
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "token.c", Path(tmp) / "token"
            src.write_text(fixture)
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-DARCH_HAS_PGTABLE_OPS=1",
                            "-DARCH_HAS_PTE_BLOCK=1", str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
