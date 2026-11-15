import importlib.util
import unittest
from pathlib import Path

PATH = Path(__file__).parents[1] / "xlator/run-idl-ack-guest.py"
SPEC = importlib.util.spec_from_file_location("idl_ack_runner", PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class AckRunnerAcceptanceTests(unittest.TestCase):
    def accepts(self, **overrides):
        values = dict(exit_code=0, sent_command=True, sent_poweroff=True,
                      found_count=10, required_count=10, forbidden_count=0,
                      image_unchanged=True, online_required=True,
                      online_seen=True)
        values.update(overrides)
        return RUNNER.acceptance_pass(**values)

    def test_requires_unchanged_image(self):
        self.assertFalse(self.accepts(image_unchanged=False))

    def test_rv64_requires_observed_online_marker(self):
        self.assertFalse(self.accepts(online_seen=False))
        self.assertTrue(self.accepts())

    def test_rv32_profile_does_not_claim_online_marker(self):
        self.assertTrue(self.accepts(online_required=False, online_seen=False))

    def test_rejects_timeout_or_missing_cleanup(self):
        self.assertFalse(self.accepts(exit_code=124))
        self.assertFalse(self.accepts(sent_poweroff=False))
        self.assertFalse(self.accepts(forbidden_count=1))


if __name__ == "__main__":
    unittest.main()
