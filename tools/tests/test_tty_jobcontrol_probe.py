"""A final PASS cannot hide a missing section, CPU or failed shutdown."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("tty_jobcontrol_probe", ROOT / "tools/probes/tty_jobcontrol_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class TtyGateOracle(unittest.TestCase):
    def test_all_sections_and_audited_shutdown(self):
        cycle = "".join(f"{prefix}: {section} PASS\n"
                        for prefix, sections in probe.SECTIONS.items() for section in sections)
        cycle += "TTY_JOBCTRL: PASS\nTTY_LIFECYCLE: PASS\nTTY_LIFECYCLE: PASS\n"
        text = "[SMP] 4/4 configured CPUs online\n" + cycle * 3 + "[SD] audit errors=0\n"
        probe.validate(text, 4, 3, True)
        for bad in (text.replace("privileged-steal PASS", "privileged-steal FAIL"),
                    text.replace("thread-sharing PASS", ""),
                    text.replace("4/4 configured", "2/4 configured"),
                    text.replace("errors=0", "errors=1"),
                    text.replace("[SD] audit errors=0", "[SD] POWER_OFF entry"),
                    text.replace("TTY_JOBCTRL: PASS", ""), text + "UBSAN: error\n"):
            with self.assertRaises(ValueError):
                probe.validate(bad, 4, 3, True)
        probe.validate(text.replace("[SD] audit errors=0", "[SD] POWER_OFF entry"), 4, 3, False)
        with self.assertRaises(ValueError):
            probe.validate(text, 4, 4, True)


if __name__ == "__main__":
    unittest.main()
