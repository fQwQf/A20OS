"""M0Sense application packaging and guarded USB upload without hardware."""
from __future__ import annotations

import importlib.util
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


pack = module("mk_m0sense_uf2")
upload = module("m0sense_deploy_uf2")
recovery = module("m0sense_recovery_assets")
INFO = "UF2 Bootloader v1.0\nModel: Sipeed M0SENSE BL702\nBoard-ID: m0sense-bl702\n"


def fixture():
    elf = bytearray(0x10c)
    elf[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", elf, 16, 2, 243, 1, pack.XIP_BASE, 52, 0, 1, 52, 32, 3, 0, 0, 0)
    segments = [(0x100, 0x23000000, 0x23000000, 4, 4, 5),
                (0x104, 0x22010000, 0x23000004, 4, 4, 5),
                (0x108, 0x42020000, 0x23000008, 4, 136, 6)]
    for i, values in enumerate(segments):
        struct.pack_into("<8I", elf, 52 + 32 * i, 1, *values, 4)
    image = b"\x73\x70\x04\x30\x13\x00\x00\x00\x50\x30\x32\x41"
    elf[0x100:] = image
    return bytes(elf), image


class Packaging(unittest.TestCase):
    def test_xip_copy_elf_and_binary_match(self):
        elf, image = fixture()
        pack.validate_application(elf, image)
        for size in (0, 51, 52, 100, len(elf) - 1):
            with self.assertRaises(ValueError):
                pack.validate_application(elf[:size], image)

    def test_bad_entry_isa_segment_and_binary_rejected(self):
        elf, image = fixture()
        changes = [(24, 0x22010000), (36, 3), (52 + 32 + 12, 0x2300ffff),
                   (52 + 64 + 8, 0x42023fff), (52 + 64 + 20, 1)]
        for offset, value in changes:
            bad = bytearray(elf)
            struct.pack_into("<I", bad, offset, value)
            with self.assertRaises(ValueError):
                pack.validate_application(bytes(bad), image)
        with self.assertRaises(ValueError):
            pack.validate_application(elf, image[:-1] + b"!")

    def test_official_format_and_payload_boundaries(self):
        for length in (1, 255, 256, 257, 4096, 65536):
            image = bytes((i % 251 for i in range(length)))
            uf2 = pack.encode_uf2(image)
            self.assertEqual(pack.decode_uf2(uf2), image.ljust((length + 255) // 256 * 256, b"\0"))
            upload.check_uf2(uf2)
            self.assertEqual(struct.unpack_from("<8I", uf2),
                (0x0a324655, 0x9e5d5157, 0x2001, 0, 256, 0, len(uf2) // 512, 0x11de784a))
        for image in (b"", bytes(65537)):
            with self.assertRaises(ValueError): pack.encode_uf2(image)

    def test_uf2_mutations_rejected(self):
        good = pack.encode_uf2(bytes(257))
        for offset in (0, 4, 8, 12, 16, 20, 24, 28, 288, 508, 512 + 12):
            bad = bytearray(good)
            bad[offset] ^= 1
            with self.assertRaises(ValueError): pack.decode_uf2(bytes(bad))
            with self.assertRaises(ValueError): upload.check_uf2(bytes(bad))
        with self.assertRaises(ValueError): pack.decode_uf2(good[:-1])


class Deployment(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="a20-m0sense-usb-test-")
        self.root = Path(self.temp.name)
        self.volume = self.root / "M0SENSE"
        self.volume.mkdir()
        (self.volume / "INFO_UF2.TXT").write_text(INFO)
        self.elf, self.image = fixture()
        (self.root / "probe.elf").write_bytes(self.elf)
        (self.root / "probe-app.bin").write_bytes(self.image)
        self.uf2 = self.root / "probe.uf2"
        pack.package(self.root / "probe.elf", self.root / "probe-app.bin", self.uf2)
        self.metadata = self.root / "probe.json"

    def tearDown(self):
        self.temp.cleanup()

    def test_dry_run_without_hardware_writes_nothing(self):
        with patch.object(upload, "mounted_fat_volumes", return_value=[]), patch.object(upload.os, "open") as op:
            result = upload.deploy(self.uf2, self.metadata, None, True)
        self.assertFalse(result["uploaded"])
        op.assert_not_called()

    def test_guarded_upload_to_fake_usb_volume(self):
        with patch.object(upload, "mounted_fat_volumes", return_value=[self.volume]):
            result = upload.deploy(self.uf2, self.metadata, self.volume, False)
        self.assertTrue(result["uploaded"])
        self.assertEqual((self.volume / "A20PROBE.UF2").read_bytes(), self.uf2.read_bytes())
        self.assertEqual((self.volume / "INFO_UF2.TXT").read_text(), INFO)

    def test_mount_identity_digest_and_existing_file_guards(self):
        with self.assertRaises(ValueError): upload.identify_volume(self.volume, [])
        link = self.root / "linked"
        link.symlink_to(self.volume, target_is_directory=True)
        with self.assertRaises(ValueError): upload.identify_volume(link, [self.volume])
        (self.volume / "INFO_UF2.TXT").write_text(INFO.replace("m0sense-bl702", "another-board"))
        with self.assertRaises(ValueError): upload.identify_volume(self.volume, [self.volume])
        (self.volume / "INFO_UF2.TXT").write_text(INFO)
        with patch.object(upload, "mounted_fat_volumes", return_value=[self.volume]):
            (self.volume / "A20PROBE.UF2").write_text("keep")
            with self.assertRaises(ValueError): upload.deploy(self.uf2, self.metadata, self.volume, False)
            self.assertEqual((self.volume / "A20PROBE.UF2").read_text(), "keep")
        metadata = json.loads(self.metadata.read_text())
        metadata["uf2_sha256"] = "0" * 64
        self.metadata.write_text(json.dumps(metadata))
        with self.assertRaises(ValueError): upload.deploy(self.uf2, self.metadata, None, True)


class RecoveryPreparation(unittest.TestCase):
    def test_dry_run_never_downloads_or_creates_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "absent"
            with patch.object(recovery.urllib.request, "urlopen") as network:
                recovery.fetch(out, True)
            network.assert_not_called()
            self.assertFalse(out.exists())

    def test_verified_download_then_offline_cached_run(self):
        data = b"official-test-fixture"
        assets = (("fixture.bin", hashlib.sha256(data).hexdigest()),)
        with tempfile.TemporaryDirectory() as tmp, patch.object(recovery, "ASSETS", assets):
            out = Path(tmp)
            with patch.object(recovery.urllib.request, "urlopen", return_value=io.BytesIO(data)) as network:
                recovery.fetch(out)
            self.assertEqual(network.call_count, 1)
            self.assertEqual((out / "fixture.bin").read_bytes(), data)
            with patch.object(recovery.urllib.request, "urlopen") as network:
                recovery.fetch(out)
            network.assert_not_called()

    def test_bad_download_or_existing_file_is_never_replaced(self):
        assets = (("fixture.bin", hashlib.sha256(b"right").hexdigest()),)
        with tempfile.TemporaryDirectory() as tmp, patch.object(recovery, "ASSETS", assets):
            out = Path(tmp)
            with patch.object(recovery.urllib.request, "urlopen", return_value=io.BytesIO(b"wrong")):
                with self.assertRaises(ValueError): recovery.fetch(out)
            self.assertFalse((out / "fixture.bin").exists())
            (out / "fixture.bin").write_bytes(b"keep")
            with self.assertRaises(ValueError): recovery.fetch(out)
            self.assertEqual((out / "fixture.bin").read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
