"""Session diagnostics survive errors and descendants retaining the output pipe."""
import importlib.util
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('session_log', ROOT / 'packages/overlay/xfce/usr/lib/a20/session-log.py')
logger = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logger)

class DesktopSessionLog(unittest.TestCase):
    def test_file_failure_does_not_prevent_desktop_start(self):
        with tempfile.TemporaryDirectory() as temp:
            kernel = Path(temp) / 'kernel'
            kernel.touch()
            result = logger.supervise([sys.executable, '-c', 'print("desktop started")'],
                                       Path(temp) / 'missing-directory/session', str(kernel))
            self.assertEqual(result, 0)
            self.assertIn(b'session file unavailable errno=', kernel.read_bytes())
            self.assertIn(b'desktop started', kernel.read_bytes())

    def test_stderr_partial_lines_and_exit_are_mirrored(self):
        with tempfile.TemporaryDirectory() as temp:
            log, kernel = Path(temp) / 'session', Path(temp) / 'kernel'
            kernel.touch()
            result = logger.supervise([sys.executable, '-c',
                'import sys;sys.stderr.write("panel error\\npartial");sys.exit(7)'], log, str(kernel))
            self.assertEqual(result, 7)
            for marker in (b'panel error', b'partial', b'status=7'):
                self.assertIn(marker, log.read_bytes())
                self.assertIn(marker, kernel.read_bytes())

    def test_owner_exit_does_not_wait_for_detached_descendant(self):
        with tempfile.TemporaryDirectory() as temp:
            start = time.monotonic()
            code = 'import subprocess,sys;subprocess.Popen([sys.executable,"-c","import time;time.sleep(2)"])'
            self.assertEqual(logger.supervise([sys.executable, '-c', code], Path(temp) / 'session',
                                               str(Path(temp) / 'missing-kmsg')), 0)
            self.assertLess(time.monotonic() - start, 1.5)

    def test_signal_exit_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'session'
            result = logger.supervise([sys.executable, '-c',
                'import os,signal;os.kill(os.getpid(),signal.SIGTERM)'], log, '/nonexistent/a20-kmsg')
            self.assertEqual(result, 128 + signal.SIGTERM)
            self.assertIn(b'status=-15', log.read_bytes())

    def test_long_utf8_and_binary_stderr_keep_json_log_text_valid(self):
        with tempfile.TemporaryDirectory() as temp:
            log, kernel = Path(temp) / 'session', Path(temp) / 'kernel'
            kernel.touch()
            program = 'import sys;sys.stderr.buffer.write(b"x"+bytes.fromhex("e4bda0")*1500+b"\\xff\\n")'
            self.assertEqual(logger.supervise([sys.executable, '-c', program], log, str(kernel)), 0)
            log.read_bytes().decode('utf-8')
            kernel.read_bytes().decode('utf-8')

if __name__ == '__main__':
    unittest.main()
