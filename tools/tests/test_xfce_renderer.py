"""The shared apk desktop chooses software rendering for firmware displays."""
import ctypes
import os
from pathlib import Path
import runpy
import struct
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SELECTOR = ROOT / "packages/overlay/xfce/usr/lib/a20/select-renderer"


class XfceRenderer(unittest.TestCase):
    def setUp(self):
        self.select = runpy.run_path(str(SELECTOR))["select_renderer"]

    def test_no_render_node_uses_pixman(self):
        with patch("os.open", side_effect=FileNotFoundError), patch("os.close") as close:
            self.assertEqual(self.select(), "pixman")
            close.assert_not_called()

    def test_firmware_kms_ioctl_fallback_closes_fd(self):
        with patch("os.open", return_value=42), patch("os.close") as close, \
                patch("fcntl.ioctl", side_effect=OSError):
            self.assertEqual(self.select(), "pixman")
            close.assert_called_once_with(42)

    def test_only_negotiated_3d_uses_gles(self):
        for features, expected in ((0, "pixman"), (1, "gles2"), (2, "pixman")):
            def ioctl(fd, command, request):
                self.assertEqual((fd, command), (42, 0xC0106443))
                param, pointer = struct.unpack("=QQ", request)
                self.assertEqual(param, 1)
                ctypes.cast(pointer, ctypes.POINTER(ctypes.c_uint64))[0] = features

            with self.subTest(features=features), patch("os.open", return_value=42) as opened, \
                    patch("os.close") as close, patch("fcntl.ioctl", side_effect=ioctl):
                self.assertEqual(self.select(), expected)
                opened.assert_called_once_with("/dev/dri/renderD128", os.O_RDWR | os.O_CLOEXEC)
                close.assert_called_once_with(42)


if __name__ == "__main__":
    unittest.main()
