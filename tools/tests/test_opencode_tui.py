#!/usr/bin/env python3
"""Small safety checks for the ordinary-TUI PTY wrapper."""
import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).parents[1] / "agents" / "opencode_tui.py"
SPEC = importlib.util.spec_from_file_location("opencode_tui", SCRIPT)
opencode_tui = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(opencode_tui)


class OutputRingTests(unittest.TestCase):
    def test_keeps_only_last_limit_bytes(self):
        ring = opencode_tui.OutputRing(limit=8)
        ring.append(b"12345")
        ring.append(b"6789")
        self.assertEqual(bytes(ring.data), b"23456789")
        self.assertLessEqual(len(ring.data), ring.limit)

    def test_oversize_chunk_keeps_its_tail(self):
        ring = opencode_tui.OutputRing(limit=4)
        ring.append(b"0123456789")
        self.assertEqual(bytes(ring.data), b"6789")


class ScreenTests(unittest.TestCase):
    def test_snapshot_tracks_visible_text_and_csi_clear(self):
        screen = opencode_tui.Screen(rows=12, cols=48)
        screen.feed(b"old\r\x1b[2K\rnew\x1b[2;3HREADY")
        snapshot = screen.snapshot()
        self.assertIn("new", snapshot)
        self.assertIn("READY", snapshot)
        self.assertNotIn("old", snapshot)

    def test_snapshot_is_bounded_to_visible_screen(self):
        screen = opencode_tui.Screen(rows=10, cols=40)
        screen.feed((("x" * 40 + "\n") * 100).encode())
        self.assertLessEqual(len(screen.snapshot().splitlines()), 48)
        self.assertLessEqual(len(screen.cells), 10)


if __name__ == "__main__":
    unittest.main()
