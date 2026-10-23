"""The guest must retain build-time public trust without the private key."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "mkrootfs", Path(__file__).resolve().parents[1] / "mkrootfs.py")
mkrootfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkrootfs)


class ApkRuntimeKeys(unittest.TestCase):
    def test_public_keys_survive_and_private_keys_are_excluded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            keys = root / "build-keys"
            keys.mkdir()
            (keys / "alpine.rsa.pub").write_text("official public key")
            (keys / "a20.rsa.pub").write_text("local public key")
            (keys / "a20.rsa").write_text("private key must stay on host")
            image = root / "image"
            mkrootfs.install_apk_keys(image, keys, [])
            installed = image / "etc/apk/keys"
            self.assertEqual(sorted(p.name for p in installed.iterdir()),
                             ["a20.rsa.pub", "alpine.rsa.pub"])
            self.assertEqual((installed / "a20.rsa.pub").read_text(), "local public key")
            self.assertEqual((installed / "a20.rsa.pub").stat().st_mode & 0o777, 0o644)

    def test_missing_optional_trust_does_not_create_a_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mkrootfs.install_apk_keys(root / "image", None, [])
            self.assertFalse((root / "image").exists())


if __name__ == "__main__":
    unittest.main()
