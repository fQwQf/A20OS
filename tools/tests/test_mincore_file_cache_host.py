"""Compile the production sys_mincore implementation against a host fixture."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected exactly one {signature!r}")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"unterminated {signature!r}")


class MincoreFileCacheHost(unittest.TestCase):
    def test_file_cache_residency_without_io(self):
        source = (ROOT / "kernel/abi/linux/sys_mm.c").read_text()
        function = extract_function(source, "int64_t sys_mincore(")
        fixture = (ROOT / "tools/tests/mincore_file_cache_host.c").read_text()
        harness = fixture.replace("/* PRODUCTION */", function)

        with tempfile.TemporaryDirectory(prefix="a20-mincore-file-cache-") as directory:
            c_source = Path(directory) / "mincore.c"
            executable = Path(directory) / "mincore-host"
            c_source.write_text(harness)
            subprocess.run(
                ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=undefined", str(c_source), "-o", str(executable)],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(executable)], check=False, text=True, capture_output=True
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("production sys_mincore", result.stdout)


if __name__ == "__main__":
    unittest.main()
