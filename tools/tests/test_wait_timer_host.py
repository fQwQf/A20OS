import pathlib
import os
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
            sizes = {}
            for capacity in (1, 4, 64, 8192):
                with self.subTest(capacity=capacity):
                    env = dict(os.environ, A20_WAIT_TIMER_STORAGE_CAPACITY=str(capacity))
                    subprocess.run([
                        "rustc", "--edition=2021", "--crate-type", "lib",
                        "--emit=obj", "-C", "panic=abort", "-C", "opt-level=2",
                        "-C", "codegen-units=1", str(ROOT / "kernel/proc/wait_timer.rs"),
                        "-o", str(obj),
                    ], check=True, cwd=ROOT, env=env)
                    subprocess.run([
                        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        *(["-DWAIT_TIMER_SINGLE_ONLY"] if capacity == 1 else []),
                        str(ROOT / "tools/tests/wait_timer_host.c"), str(obj),
                        "-o", str(binary),
                    ], check=True, cwd=ROOT)
                    result = subprocess.run([str(binary)], check=True, text=True,
                                            capture_output=True)
                    self.assertIn("WAIT_TIMER_HOST: PASS", result.stdout)
                    symbols = subprocess.check_output(["nm", "-S", "--defined-only", str(obj)], text=True)
                    heaps = [line.split() for line in symbols.splitlines() if "HEAP" in line]
                    self.assertEqual(len(heaps), 1)
                    sizes[capacity] = int(heaps[0][1], 16)
            # Check emitted storage, not merely the reported runtime limit.
            self.assertGreater(sizes[8192] - sizes[4], 128 * 1024)
            self.assertLess(sizes[4], 1024)
            self.assertLess(sizes[64], sizes[8192])

    def test_invalid_storage_capacity_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="a20-wait-timer-invalid-") as tmp:
            for capacity in ("0", "8193", "invalid"):
                with self.subTest(capacity=capacity):
                    result = subprocess.run([
                        "rustc", "--edition=2021", "--crate-type", "lib", "--emit=obj",
                        "-C", "panic=abort", str(ROOT / "kernel/proc/wait_timer.rs"),
                        "-o", str(pathlib.Path(tmp) / "invalid.o"),
                    ], env=dict(os.environ, A20_WAIT_TIMER_STORAGE_CAPACITY=capacity),
                        capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("wait timer capacity", result.stderr)


if __name__ == "__main__":
    unittest.main()
