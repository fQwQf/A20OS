import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class WaitTimerHostTest(unittest.TestCase):
    def test_production_rust_heap_ownership_and_expire_race(self):
        with tempfile.TemporaryDirectory(prefix="a20-wait-timer-") as tmp:
            tmpdir = pathlib.Path(tmp)
            obj = tmpdir / "wait_timer.o"
            binary = tmpdir / "wait_timer_host"
            subprocess.run([
                "rustc", "--edition=2021", "--crate-type", "lib",
                "--emit=obj", "-C", "panic=abort", "-C", "opt-level=2",
                "-C", "codegen-units=1", str(ROOT / "kernel/proc/wait_timer.rs"),
                "-o", str(obj),
            ], check=True, cwd=ROOT)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(ROOT / "tools/tests/wait_timer_host.c"), str(obj),
                "-o", str(binary),
            ], check=True, cwd=ROOT)
            result = subprocess.run([str(binary)], check=True, text=True,
                                    capture_output=True)
            self.assertIn("WAIT_TIMER_HOST: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
