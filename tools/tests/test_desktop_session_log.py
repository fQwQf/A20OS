"""Session diagnostics survive errors and descendants retaining the output pipe."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('session_log', ROOT / 'packages/overlay/xfce/usr/lib/a20/session-log.py')
logger = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logger)

class DesktopSessionLog(unittest.TestCase):
    logger_source = ROOT / 'packages/overlay/xfce/usr/lib/a20/session-log.py'

    def _launch(self, code, log, kernel, stop_timeout=None):
        program = '''import runpy,sys
supervise=runpy.run_path(sys.argv[1])["supervise"]
options={} if not sys.argv[5] else {"stop_timeout":float(sys.argv[5])}
raise SystemExit(supervise([sys.executable,"-c",sys.argv[2]],sys.argv[3],sys.argv[4],**options))
'''
        return subprocess.Popen([sys.executable, '-c', program, str(self.logger_source),
            code, str(log), str(kernel), '' if stop_timeout is None else str(stop_timeout)],
            start_new_session=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def _wait_for(self, log, *markers):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            data = log.read_bytes() if log.exists() else b''
            if all(marker in data for marker in markers):
                return data
            time.sleep(0.01)
        self.fail('session startup timed out: ' + repr(data))

    def _cleanup(self, process, log):
        # Each test owns a separate session. Clean a failed negative control's
        # still-live owner as well as the supervisor; never touch the test runner.
        groups = {process.pid}
        if log.exists():
            for line in log.read_text().splitlines():
                if line.startswith('session child started pid='):
                    pid = int(line.split('=')[1])
                    try:
                        groups.add(os.getpgid(pid))
                    except ProcessLookupError:
                        pass
        for group in groups:
            self.assertNotEqual(group, os.getpgrp())
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.communicate(timeout=3)

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

    def test_term_reaches_owner_and_worker_before_logger_exits(self):
        worker = '''import signal,sys,time
def stop(signum,frame):
 print("worker cleanup",flush=True)
 raise SystemExit(0)
signal.signal(signal.SIGTERM,stop)
print("worker ready",flush=True)
while True: time.sleep(1)
'''
        owner = '''import signal,subprocess,sys,time
worker=subprocess.Popen([sys.executable,"-c",%r])
def stop(signum,frame):
 print("owner cleanup",flush=True)
 worker.wait(timeout=2)
 raise SystemExit(0)
signal.signal(signal.SIGTERM,stop)
print("owner ready",flush=True)
while True: time.sleep(1)
''' % worker
        with tempfile.TemporaryDirectory() as temp:
            log, kernel = Path(temp) / 'session', Path(temp) / 'kernel'
            kernel.touch()
            process = self._launch(owner, log, kernel)
            try:
                self._wait_for(log, b'owner ready', b'worker ready')
                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=3)
                self.assertEqual(process.returncode, 0, output)
                for path in (log, kernel):
                    data = path.read_bytes()
                    for marker in (b'stop requested signal=15', b'owner cleanup',
                                   b'worker cleanup', b'session child exited status=0'):
                        self.assertIn(marker, data)
            finally:
                self._cleanup(process, log)

    def test_closed_output_does_not_hide_live_owner_or_stop_timeout(self):
        owner = '''import os,signal,time
signal.signal(signal.SIGTERM,signal.SIG_IGN)
print("owner ready",flush=True)
os.close(1);os.close(2)
while True: time.sleep(1)
'''
        with tempfile.TemporaryDirectory() as temp:
            log, kernel = Path(temp) / 'session', Path(temp) / 'kernel'
            kernel.touch()
            process = self._launch(owner, log, kernel, stop_timeout=0.15)
            try:
                self._wait_for(log, b'owner ready')
                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=2)
                self.assertEqual(process.returncode, 128 + signal.SIGKILL, output)
                self.assertIn(b'stop timed out; sending SIGKILL', log.read_bytes())
                self.assertIn(b'session child exited status=-9', log.read_bytes())
            finally:
                self._cleanup(process, log)

if __name__ == '__main__':
    unittest.main()
