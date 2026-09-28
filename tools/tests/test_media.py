"""Tests for the physical-media writer (tools/media.py).

`dd` to a block node is the only irreversible command in the build system, so
these tests exist to pin the guards rather than the copy.  They are hermetic:
the block-device *predicate* is stubbed where a real loop device would be
needed, because creating one needs privileges a test runner does not have, and
the consequence of that limitation is called out at each stub.

Stdlib unittest, matching tools/tests/test_a20.py: `tools/a20` promises stdlib
only, so a pytest requirement would break that promise for every contributor.
"""

from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import media  # noqa: E402


@contextlib.contextmanager
def quiet_dd():
    """Silence dd's progress output.

    Only the file descriptor is redirected: dd is a subprocess writing straight
    to fd 2, which contextlib.redirect_stderr cannot reach.  Python's own
    streams are deliberately left alone so a caller can still capture them.
    """
    saved = os.dup(2)
    devnull = os.open(os.devnull, os.O_WRONLY)
    try:
        os.dup2(devnull, 2)
        yield
    finally:
        os.dup2(saved, 2)
        os.close(devnull)
        os.close(saved)


def run_main(boot_media: str, device: str) -> tuple[int, str]:
    """Run media.main() and capture what it reported to the user."""
    argv = ["media.py", "--boot-media", boot_media, "--media-device", device]
    out = io.StringIO()
    with patch.object(sys, "argv", argv), contextlib.redirect_stderr(out), \
            contextlib.redirect_stdout(io.StringIO()), quiet_dd():
        rc = media.main()
    return rc, out.getvalue()


class TestGuards(unittest.TestCase):
    def test_empty_boot_media_refuses(self) -> None:
        rc, err = run_main("", "/dev/null")
        self.assertEqual(rc, 1)
        self.assertIn("TARGET_BOOT_MEDIA is empty", err)

    def test_empty_media_device_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "a.img"
            img.write_bytes(b"x")
            rc, err = run_main(str(img), "")
        self.assertEqual(rc, 1)
        self.assertIn("TARGET_MEDIA_DEVICE is empty", err)

    def test_regular_file_is_not_a_block_device(self) -> None:
        # Unstubbed: this is the guard that stops a mistyped node from
        # destroying the host's disk, so it must be tested for real.
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "a.img"
            img.write_bytes(b"x")
            decoy = Path(d) / "decoy"
            decoy.write_bytes(b"not a disk")
            rc, err = run_main(str(img), str(decoy))
        self.assertEqual(rc, 1)
        self.assertIn("is not a block device", err)
        self.assertIn("mistyped node", err)

    def test_mounted_device_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "a.img"
            img.write_bytes(b"x")
            with patch.object(media, "is_mounted", return_value=True), \
                    patch.object(media, "check_block_device", return_value=None):
                rc, err = run_main(str(img), "/dev/sdX")
        self.assertEqual(rc, 1)
        self.assertIn("is mounted; unmount it first", err)

    def test_missing_image_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            present = Path(d) / "a.img"
            present.write_bytes(b"x")
            absent = Path(d) / "gone.img"
            with patch.object(media, "check_block_device", return_value=None):
                rc, err = run_main(f"{present} {absent}", str(Path(d) / "dev"))
        self.assertEqual(rc, 1)
        self.assertIn("missing image", err)


class TestSafetyProperties(unittest.TestCase):
    """Regressions for the two behaviours the original shell got wrong."""

    def test_all_images_validated_before_any_write(self) -> None:
        # The original checked each image inside the write loop, so a missing
        # second image left the device carrying the first one.  Validation must
        # complete before the device is touched.
        with tempfile.TemporaryDirectory() as d:
            first = Path(d) / "a.img"
            first.write_bytes(b"A" * 4096)
            dev = Path(d) / "dev"
            with patch.object(media, "check_block_device", return_value=None):
                rc, err = run_main(f"{first} {d}/missing.img", str(dev))
            self.assertEqual(rc, 1)
            self.assertFalse(dev.exists(), "device was written despite a later missing image")
            self.assertIn("missing image", err)

    def test_write_failure_is_not_reported_as_success(self) -> None:
        # The original ran `dd` without checking it, so a failed flash still
        # printed "target-write-media: done" and exited 0.
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "a.img"
            img.write_bytes(b"A" * 4096)
            dev = Path(d) / "dev"
            with patch.object(media, "check_block_device", return_value=None):
                rc, out = run_main(str(img), str(dev))
        self.assertEqual(rc, 0)

        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "a.img"
            img.write_bytes(b"A" * 4096)
            with quiet_dd(), contextlib.redirect_stdout(io.StringIO()):
                rc = media.write("/nonexistent-dir/dev", img)
        self.assertNotEqual(rc, 0, "write() must propagate dd's failure")


class TestWrite(unittest.TestCase):
    def test_bytes_are_copied_verbatim(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            payload = os.urandom(300_000)
            src = Path(d) / "src.img"
            dst = Path(d) / "dst.img"
            src.write_bytes(payload)
            with quiet_dd(), contextlib.redirect_stdout(io.StringIO()):
                rc = media.write(str(dst), src)
            self.assertEqual(rc, 0)
            self.assertEqual(dst.read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
