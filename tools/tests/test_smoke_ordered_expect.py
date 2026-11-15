import unittest
import tempfile
from contextlib import redirect_stdout
from io import StringIO

from tools.smoke import (_ordered_expect_missing,
                         _ordered_forbid_before_violations,
                         _exit_status_missing,
                         report)
from tools.smoke import CASES


class OrderedExpectationTests(unittest.TestCase):
    def test_nommu_gate_requires_child_output_not_command_echo(self):
        case = dict(CASES['smoke-nommu-exec'])
        for output, status, wanted in (
            ("# /bin/echo NOMMU_MARK\n# /bin/echo NOMMU_SECOND\n"
             "[SD] POWER_OFF entry\n", 0, 1),
            ("NOMMU_MARK\r\nNOMMU_SECOND\r\n[SD] POWER_OFF entry\n", 0, 0),
            ("NOMMU_MARK\nNOMMU_SECOND\n[SD] POWER_OFF entry\n", 124, 1),
        ):
            with self.subTest(output=output, status=status), tempfile.NamedTemporaryFile(mode='w+') as f:
                f.write(output)
                f.flush()
                case['log'] = f.name
                with redirect_stdout(StringIO()):
                    self.assertEqual(report('smoke-nommu-exec', case, status), wanted)

    def test_allows_expected_sequence(self):
        patterns = [r"boundary call=5", r"source-presplit-error split=5",
                    r"RETRY: begin split=5", r"phase=enter", r"PASS split=5"]
        text = "\n".join([
            "boundary call=5 would_allocate=1",
            "source-presplit-error split=5 rc=-12",
            "RETRY: begin split=5",
            "MM_DONTUNMAP_TXN_TEST: impl=rust phase=enter",
            "PASS split=5",
        ])
        self.assertEqual(_ordered_expect_missing(patterns, text), [])

    def test_rejects_retry_enter_that_precedes_initial_error(self):
        patterns = [r"source-presplit-error split=5", r"RETRY: begin split=5",
                    r"phase=enter", r"PASS split=5"]
        text = "\n".join([
            "MM_DONTUNMAP_TXN_TEST: impl=rust phase=enter",
            "source-presplit-error split=5 rc=-12",
            "RETRY: begin split=5",
            "MM_DONTUNMAP_TXN_TEST: impl=rust phase=enter",
            "PASS split=5",
        ])
        self.assertEqual(_ordered_expect_missing(patterns, text), [])
        self.assertEqual(_ordered_forbid_before_violations([{
            "before": r"source-presplit-error split=5",
            "forbid": r"MM_DONTUNMAP_TXN_TEST: impl=(c|rust) phase=",
        }], text), [r"MM_DONTUNMAP_TXN_TEST: impl=(c|rust) phase="])

    def test_markers_do_not_override_nonzero_or_timeout_status(self):
        self.assertEqual(_exit_status_missing(True, 0), None)
        self.assertEqual(_exit_status_missing(True, 1), "expected exit status 0, got 1")
        self.assertEqual(_exit_status_missing(True, 124), "expected exit status 0, got 124")
        self.assertEqual(_exit_status_missing(False, 124), None)
        with tempfile.NamedTemporaryFile(mode="w+", encoding="utf-8") as f:
            f.write("CONTRACT: PASS\n")
            f.flush()
            case = {"log": f.name, "expect": ["CONTRACT: PASS"],
                    "require_exit_zero": True, "pass_msg": "PASS $log"}
            with redirect_stdout(StringIO()):
                self.assertEqual(report("status-zero", case, 0), 0)
            for status in (1, 124):
                with redirect_stdout(StringIO()):
                    self.assertEqual(report("nonzero-status", case, status), 1)


if __name__ == "__main__":
    unittest.main()
