"""Tests for the media-overlay staging in tools/pkg.py (cmd_media_overlay).

GUI_MEDIA copies arbitrary host media into an overlay that mkrootfs merges
into the image.  The Mojang layout is the one media type that carries ELF:
its natives/ are published for x86_64 (arm64 on newer versions) and never
for riscv64, and mkrootfs' overlay check rejects an image that ships a
foreign-architecture ELF.  These tests pin the contract that turned that
rejection into a filtered copy: foreign ELFs are dropped with a log line,
everything else is copied verbatim, and a directory whose files were all
dropped does not survive as an empty husk (the guest launcher's presence
check would sail past it and die later in dlopen with no explanation).

Stdlib unittest, matching tools/tests/test_a20.py.
"""

from __future__ import annotations

import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import mkrootfs  # noqa: E402
import pkg  # noqa: E402


def elf(e_machine: int, ei_data: int = 1) -> bytes:
    """A minimal 20-byte ELF header, e_machine little-endian at offset 18."""
    ident = b"\x7fELF" + bytes([2, ei_data, 1]) + b"\0" * 9
    return ident + struct.pack("<H", 1) + struct.pack("<H", e_machine)


class TestMediaOverlay(unittest.TestCase):
    """Each test builds its own media tree; nothing here touches build/."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def stage(self, name: str = "1.21.11") -> Path:
        media = self.tmp / "media" / name
        (media / "natives").mkdir(parents=True)
        (media / "natives" / "liblwjgl.so").write_bytes(elf(62))       # x86_64
        (media / "natives" / "libopenal.so").write_bytes(elf(62))
        (media / "client.jar").write_bytes(b"PK\x03\x04 not an ELF")
        (media / "assets").mkdir()
        (media / "assets" / "sound.ogg").write_bytes(b"OggS")
        return media

    def overlay_for(self, media: Path, arch: str):
        """Stage `media` (dir or file); return (overlay root, /usr/share/a20-media, stdout)."""
        overlay = self.tmp / f"overlay-{arch or 'none'}"
        out = io.StringIO()
        args = SimpleNamespace(media=str(media), media_dir="/usr/share/a20-media",
                               media_overlay=str(overlay), arch=arch)
        with contextlib.redirect_stdout(out):
            rc = pkg.cmd_media_overlay(args)
        self.assertEqual(rc, 0)
        return overlay, overlay / "usr/share/a20-media", out.getvalue()

    def test_foreign_natives_dropped_for_riscv64(self) -> None:
        media = self.stage()
        _, media_root, out = self.overlay_for(media, "riscv64")
        staged = media_root / media.name
        self.assertFalse((staged / "natives").exists(),
                         "natives/ must not survive as an empty directory")
        self.assertEqual((staged / "client.jar").read_bytes(),
                         (media / "client.jar").read_bytes())
        self.assertEqual((staged / "assets" / "sound.ogg").read_bytes(),
                         (media / "assets" / "sound.ogg").read_bytes())
        self.assertIn("dropped 2 ELF file(s)", out)
        self.assertIn("target riscv64", out)
        self.assertIn("natives/liblwjgl.so", out)

    def test_matching_natives_kept_for_x86_64(self) -> None:
        media = self.stage()
        _, media_root, out = self.overlay_for(media, "x86_64")
        self.assertEqual(
            sorted(p.name for p in (media_root / media.name / "natives").iterdir()),
            ["liblwjgl.so", "libopenal.so"])
        self.assertNotIn("dropped", out)

    def test_unknown_arch_filters_nothing(self) -> None:
        media = self.stage()
        _, media_root, out = self.overlay_for(media, "")
        self.assertTrue((media_root / media.name / "natives" / "libopenal.so").exists())
        self.assertNotIn("dropped", out)

    def test_desktop_symlink_created(self) -> None:
        media = self.stage()
        overlay_root, _, _ = self.overlay_for(media, "riscv64")
        link = overlay_root / "root" / "Desktop" / "a20-media"
        self.assertTrue(link.is_symlink())
        self.assertEqual(Path(link.readlink()).as_posix(), "/usr/share/a20-media")

    def test_single_foreign_file_dropped(self) -> None:
        foreign = self.tmp / "libx.so"
        foreign.write_bytes(elf(62))
        _, media_root, out = self.overlay_for(foreign, "riscv64")
        self.assertFalse((media_root / "libx.so").exists())
        self.assertIn("dropped 1 ELF file(s)", out)
        self.assertIn("libx.so", out)

    def test_single_data_file_copied(self) -> None:
        video = self.tmp / "demo.mp4"
        video.write_bytes(b"\x00\x00\x00\x18ftypmp42")
        _, media_root, out = self.overlay_for(video, "riscv64")
        self.assertEqual((media_root / "demo.mp4").read_bytes(), video.read_bytes())
        self.assertNotIn("dropped", out)

    def test_missing_media_fails_loudly(self) -> None:
        args = SimpleNamespace(media=str(self.tmp / "gone"), media_dir="/m",
                               media_overlay=str(self.tmp / "ov"), arch="riscv64")
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit):
                pkg.cmd_media_overlay(args)


class TestElfMachine(unittest.TestCase):
    """The header reader pkg.py and check_overlay_elf_arch share."""

    def test_reads_little_endian_machine(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.so"
            p.write_bytes(elf(62))
            self.assertEqual(mkrootfs.elf_machine(p), 62)
            p.write_bytes(elf(243))
            self.assertEqual(mkrootfs.elf_machine(p), 243)

    def test_rejects_non_elf_and_big_endian(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.bin"
            p.write_bytes(b"PK\x03\x04 nothing elfish here at all")
            self.assertIsNone(mkrootfs.elf_machine(p))
            p.write_bytes(elf(62, ei_data=2))
            self.assertIsNone(mkrootfs.elf_machine(p))

    def test_riscv64_is_243(self) -> None:
        # The e_machine table is the one fact three tools agree on; pin the
        # pair this bug was about.
        self.assertEqual(mkrootfs.ELF_MACHINES["riscv64"], 243)
        self.assertEqual(mkrootfs.ELF_MACHINES["x86_64"], 62)


if __name__ == "__main__":
    unittest.main()
