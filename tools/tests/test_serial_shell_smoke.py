"""The serial-shell oracle requires executed output, never terminal input echo."""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
try:
    spec = importlib.util.spec_from_file_location("serial_shell_smoke", ROOT / "tools/check_serial_shell.py")
    smoke = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(smoke)
finally:
    sys.path.pop(0)


class SerialShellOracleTest(unittest.TestCase):
    def test_terminal_command_echo_cannot_satisfy_markers(self):
        text = "\n".join(smoke.commands("poweroff"))
        for marker in smoke.MARKERS:
            self.assertNotIn(marker, text)
        with self.assertRaises(ValueError):
            smoke.validate_output(text + "\nSystem is going down for power-off NOW.\n", 0, "poweroff")

    def test_executed_output_and_both_shutdown_modes(self):
        for mode, final in (("poweroff", "System is going down for power-off NOW."),
                            ("reboot", "Rebooting system.")):
            smoke.validate_output("\r\n".join((*smoke.MARKERS, final)), 0, mode)

    def test_failed_guest_and_kernel_diagnostics_fail(self):
        text = "\n".join((*smoke.MARKERS, "System is going down for power-off NOW."))
        for status, extra in ((1, ""), (0, "\nKERNEL PANIC"), (0, "\nUBSAN: overflow"), (0, "\nSIGSEGV")):
            with self.assertRaises(ValueError):
                smoke.validate_output(text + extra, status, "poweroff")

    def test_missing_file_read_output_fails(self):
        text = "\n".join(marker for marker in smoke.MARKERS if marker != "A20_FILE_CONTENT")
        with self.assertRaises(ValueError):
            smoke.validate_output(text + "\nSystem is going down for power-off NOW.\n", 0, "poweroff")


if __name__ == "__main__":
    unittest.main()
