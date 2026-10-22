"""Check sparse fork's real C walkers against their previous range contract."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ForkRange(unittest.TestCase):
    def test_sparse_policy_huge_swap_and_failure(self):
        text = (ROOT / "kernel/mm/cow.c").read_text()
        start = text.index("static int mm_fork_clone_level(")
        end = text.index("#else /* CONFIG_NOMMU */", start)
        fixture = (ROOT / "tools/tests/cow_clone_range_host.c").read_text()
        harness = fixture.replace("/* PRODUCTION */", text[start:end])
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "range.c", Path(tmp) / "range"
            src.write_text(harness)
            for flags in [[], ["-DCONFIG_SWAP=1"]]:
                subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                                "-Wno-misleading-indentation", "-fsanitize=undefined",
                                *flags, str(src), "-o", str(exe)], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
