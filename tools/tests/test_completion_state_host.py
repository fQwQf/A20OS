import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class CompletionStateHostTest(unittest.TestCase):
    def test_production_rust_counter_and_wait_token_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="a20-completion-") as tmp:
            tmpdir = pathlib.Path(tmp)
            obj = tmpdir / "completion_state.o"
            binary = tmpdir / "completion_state_host"
            subprocess.run([
                "rustc", "--edition=2021", "--crate-type=lib", "--emit=obj",
                "-C", "panic=abort", "-C", "opt-level=2", "-C",
                "codegen-units=1", str(ROOT / "kernel/core/completion_state.rs"),
                "-o", str(obj),
            ], check=True, cwd=ROOT)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(ROOT / "tools/tests/completion_state_host.c"), str(obj),
                "-o", str(binary),
            ], check=True, cwd=ROOT)
            result = subprocess.run([str(binary)], check=True, text=True,
                                    capture_output=True)
            self.assertIn("COMPLETION_HOST: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
