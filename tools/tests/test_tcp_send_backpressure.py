"""Exercise the real external TCP send loop with allocation/output failures.

The loop moved from C to kernel/net/socket_inet_send_tcp.rs.  Run that file's
own host tests instead of scraping a function body: the tests script tcp_write
returns per attempt, so partial acceptance and the retry/backpressure branches
are distinguished from a plain implementation regression.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'kernel/net/socket_inet_send_tcp.rs'


class TcpSendBackpressure(unittest.TestCase):
    def test_accepted_bytes_and_retry(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'tcp_send_tests'
            subprocess.run(['rustc', '--edition=2021', '--test', str(SOURCE),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary), '--test-threads=1'], check=True)


if __name__ == '__main__':
    unittest.main()
