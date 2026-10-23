import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SELECTOR = ROOT / "packages/overlay/xfce/usr/lib/a20/minecraft/select.py"
spec = importlib.util.spec_from_file_location("minecraft_select", SELECTOR)
selector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selector)


class MinecraftSelection(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.media = Path(self.temp.name)

    def client(self, name, arch):
        home = self.media / name
        (home / "natives").mkdir(parents=True)
        (home / "client.jar").touch()
        (home / "version.json").write_text(json.dumps({"id": "1.21.11"}))
        header = b"\x7fELF\x02\x01" + bytes(12) + selector.MACHINES[arch].to_bytes(2, "little")
        (home / "natives/liblwjgl.so").write_bytes(header)
        return home

    def run_selector(self, arch, *extra):
        return subprocess.run(["python3", str(SELECTOR), "--media", str(self.media),
                               "--arch", arch, *extra], text=True, capture_output=True)

    def test_mixed_architectures_and_custom_directory_names(self):
        native = self.client("1.21.11", "x86_64")
        arm = self.client("arbitrary-arm-client", "aarch64")
        rv = self.client("1.21.11-riscv64", "riscv64")
        for arch, expected in (("x86_64", native), ("aarch64", arm), ("riscv64", rv)):
            result = self.run_selector(arch)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), str(expected))
            self.assertEqual(self.run_selector(arch, "--all").stdout.strip(), str(expected))

    def test_reject_foreign_explicit_home_and_stripped_natives(self):
        foreign = self.client("client", "riscv64")
        self.assertNotEqual(self.run_selector("x86_64", "--home", str(foreign)).returncode, 0)
        (foreign / "natives/liblwjgl.so").unlink()
        self.assertNotEqual(self.run_selector("riscv64").returncode, 0)

    def test_version_comes_from_manifest(self):
        self.client("1.21.11-riscv64", "riscv64")
        self.assertEqual(self.run_selector("riscv64", "--version").stdout.strip(), "1.21.11")

    def test_numeric_order_and_missing_arch(self):
        self.client("1.9", "x86_64")
        newest = self.client("1.10", "x86_64")
        self.assertEqual(self.run_selector("x86_64").stdout.strip(), str(newest))
        self.assertEqual(self.run_selector("loongarch64", "--all").stdout.strip(), "")
        self.assertNotEqual(self.run_selector("loongarch64").returncode, 0)


if __name__ == "__main__":
    unittest.main()
