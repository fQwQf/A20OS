"""Reject incomplete recovery evidence, even when the final PASS is present."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "aa64_supervision_probe", ROOT / "tools/probes/aarch64_supervision_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

BASE = "[SMP] 4/4 configured CPUs online\n[SD] audit errors=0\n"
CYCLE = """NATIVE_REGISTRY: PASS
NATIVE_REGISTRY: echod hang silent=PASS
NATIVE_REGISTRY: echod healed echo=PASS
NATIVE_REGISTRY: echod alive after rejected kill=PASS
NATIVE_REGISTRY: foreign kill rejected st=-5
SVC_MGR: health ping timeout name=echod
SVC_MGR: crash detected name=echod exit_code=-9
"""


class SupervisionOracle(unittest.TestCase):
    def test_repeated_recovery_and_raw_native_exit_status(self):
        text = BASE + "UFSD: serving /ufs as fat\n" + CYCLE * 3
        probe.validate(text, "supervisor", 3)
        probe.validate(text.replace("exit_code=-9", "exit_code=18446744073709551607"),
                       "supervisor", 3)
        for bad in (text.replace("exit_code=-9", "exit_code=42"),
                    text.replace("exit_code=-9", "exit_code=9"),
                    text.replace("foreign kill rejected st=-5", "foreign kill rejected st=0"),
                    text.replace("NATIVE_REGISTRY: echod healed echo=PASS", ""),
                    text.replace("[SD] audit errors=0", "[SD] audit errors=1"),
                    text + "SVC_MGR: FLAP budget exceeded\n"):
            with self.assertRaises(ValueError):
                probe.validate(bad, "supervisor", 3)
        with self.assertRaises(ValueError):
            probe.validate(BASE + "UFSD: serving /ufs as fat\n" + CYCLE, "supervisor", 3)

    def test_rtc_requires_two_real_alarms_and_restarts(self):
        text = BASE + "NATIVE_SVC: PASS restarts=2\n" + (
            "NATIVE_RTCD: alarm_ms=2000\nNATIVE_RTCD: PASS\n") * 2
        probe.validate(text, "rtc", 3)
        for bad in (text.replace("alarm_ms=2000", "alarm_ms=1"),
                    text.replace("alarm_ms=2000", "alarm_ms=5001"),
                    text.replace("NATIVE_RTCD: PASS", "NATIVE_RTCD: FAIL"),
                    text.replace("4/4 configured", "1/4 configured")):
            with self.assertRaises(ValueError):
                probe.validate(bad, "rtc", 3)


if __name__ == "__main__":
    unittest.main()
