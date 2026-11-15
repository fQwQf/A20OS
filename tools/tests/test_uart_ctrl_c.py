import importlib.util
from pathlib import Path
import unittest

PATH = Path(__file__).parents[1] / 'qemu/check_uart_ctrl_c.py'
SPEC = importlib.util.spec_from_file_location('uart_ctrl_c', PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class UartCtrlCAcceptance(unittest.TestCase):
    def test_requires_irq_bytes_without_polled_bytes_and_bounded_clean_exit(self):
        self.assertTrue(RUNNER.accepted(.01, 1, 0, 0, True))
        for elapsed, irq, poll, exited, clean in (
            (2, 1, 0, 0, True), (.01, 0, 1, 0, True),
            (.01, 1, 1, 0, True), (.01, 1, 0, None, True),
            (.01, 1, 0, 1, True), (.01, 1, 0, 0, False),
        ):
            with self.subTest(elapsed=elapsed, irq=irq, poll=poll, exited=exited, clean=clean):
                self.assertFalse(RUNNER.accepted(elapsed, irq, poll, exited, clean))


if __name__ == '__main__':
    unittest.main()
