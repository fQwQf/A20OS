"""Host tool resolution tests for mkrootfs, without invoking a formatter."""

from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import mkrootfs  # noqa: E402


class TestMkfsExt4Resolution(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.path_dir = self.tmp / "path-bin"
        self.path_dir.mkdir()
        self.sbin_dir = self.tmp / "sbin"
        self.sbin_dir.mkdir()

    def executable(self, directory: Path) -> Path:
        tool = directory / "mkfs.ext4"
        tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        tool.chmod(0o755)
        return tool

    def test_current_path_has_priority(self) -> None:
        path_tool = self.executable(self.path_dir)
        self.executable(self.sbin_dir)
        with mock.patch.dict(os.environ, {"PATH": str(self.path_dir)}), \
             mock.patch.object(mkrootfs, "MKFS_EXT4_FALLBACK_DIRS",
                               (self.sbin_dir,)):
            self.assertEqual(mkrootfs.resolve_mkfs_ext4(), str(path_tool.resolve()))

    def test_standard_sbin_fallback(self) -> None:
        sbin_tool = self.executable(self.sbin_dir)
        with mock.patch.dict(os.environ, {"PATH": str(self.path_dir)}), \
             mock.patch.object(mkrootfs, "MKFS_EXT4_FALLBACK_DIRS",
                               (self.sbin_dir,)):
            self.assertEqual(mkrootfs.resolve_mkfs_ext4(), str(sbin_tool.resolve()))

    def test_non_executable_is_rejected(self) -> None:
        tool = self.sbin_dir / "mkfs.ext4"
        tool.write_text("not executable", encoding="utf-8")
        tool.chmod(0o644)
        with mock.patch.dict(os.environ, {"PATH": str(self.path_dir)}), \
             mock.patch.object(mkrootfs, "MKFS_EXT4_FALLBACK_DIRS",
                               (self.sbin_dir,)):
            with contextlib.redirect_stderr(io.StringIO()) as stderr:
                with self.assertRaises(SystemExit) as raised:
                    mkrootfs.resolve_mkfs_ext4()
        self.assertEqual(raised.exception.code, 1)
        self.assertIn("mkfs.ext4 not found or not executable", stderr.getvalue())

    def test_missing_tool_fails_before_apk_or_staging(self) -> None:
        empty_path = str(self.path_dir)
        argv = ["mkrootfs.py", "--arch", "x86_64", "--world",
                str(self.tmp / "world"), "--no-alpine", "--usermode"]
        with mock.patch.dict(os.environ, {"PATH": empty_path}), \
             mock.patch.object(mkrootfs, "MKFS_EXT4_FALLBACK_DIRS", ()), \
             mock.patch.object(sys, "argv", argv), \
             mock.patch.object(mkrootfs.subprocess, "run") as run:
            with contextlib.redirect_stderr(io.StringIO()) as stderr:
                with self.assertRaises(SystemExit) as raised:
                    mkrootfs.main()
        self.assertEqual(raised.exception.code, 1)
        self.assertIn("required host tool mkfs.ext4", stderr.getvalue())
        self.assertNotIn("staging at", stderr.getvalue())
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
