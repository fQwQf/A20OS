"""The factory UF2 loader restarts before a serial verifier can attach late."""
from contextlib import redirect_stdout
import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from test_hardware_deployment import a20_module
from a20_instance import parse_instance
from a20_console import SessionResult
from a20_error import A20Error


class M0SenseCLI(unittest.TestCase):
    def args(self):
        return SimpleNamespace(auto_handoff=False, no_build=True, no_console=False,
            dry_run=False, uf2_mount="/media/test-M0SENSE", serial="/dev/ttyUSB9", make_args=[])

    def test_serial_opens_before_usb_upload_and_same_transport_verifies(self):
        module = a20_module()
        inst = parse_instance(Path(__file__).resolve().parents[2] / "instances/m0sense-bl702-probe.toml")
        events = []
        transport = SimpleNamespace(close=lambda: events.append("close"))
        def upload(*args):
            self.assertEqual(events, ["open"])
            self.assertEqual(args[1].name, "probe.json")
            events.append("upload")
        def verify(actual_transport, actual_instance, **kwargs):
            self.assertIs(actual_transport, transport)
            self.assertEqual(actual_instance.target.serial, "/dev/ttyUSB9")
            self.assertEqual(events, ["open", "upload"])
            events.append("verify")
            return SessionResult(True, "done")
        with patch.object(module, "open_transport", side_effect=lambda _: events.append("open") or transport), \
                patch("m0sense_deploy_uf2.deploy", side_effect=upload), \
                patch.object(module, "run_console_session", side_effect=verify), \
                patch.object(module, "_report", return_value=0), redirect_stdout(io.StringIO()):
            self.assertEqual(module._deploy_m0sense(inst, self.args()), 0)
        self.assertEqual(events, ["open", "upload", "verify", "close"])

    def test_failed_upload_closes_preopened_uart_without_claiming_boot(self):
        module = a20_module()
        inst = parse_instance(Path(__file__).resolve().parents[2] / "instances/m0sense-bl702-probe.toml")
        events = []
        transport = SimpleNamespace(close=lambda: events.append("close"))
        with patch.object(module, "open_transport", return_value=transport), \
                patch("m0sense_deploy_uf2.deploy", side_effect=ValueError("wrong USB identity")), \
                patch.object(module, "run_console_session") as verify:
            with self.assertRaises(A20Error):
                module._deploy_m0sense(inst, self.args())
            verify.assert_not_called()
        self.assertEqual(events, ["close"])


if __name__ == "__main__":
    unittest.main()
