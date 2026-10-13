import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "run_opencode_v2", ROOT / "tools/agents/run_opencode_v2.py"
)
runner = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(runner)


class OpenCodeV2RunnerTests(unittest.TestCase):
    def test_free_price_and_limited_time_copy_must_both_be_present(self):
        catalog = json.dumps({"data": [{"id": "step-5-preview-free", "owned_by": "opencode"}]}).encode()
        pricing = b"""<table><tr><th>Model</th><th>Input</th><th>Output</th><th>Cache read</th><th>Cache write</th></tr>
        <tr><td>Step 5 Preview Free</td><td>Free</td><td>Free</td><td>Free</td><td>-</td></tr></table>
        <p>Step 5 Preview Free is free on OpenCode for a limited time.</p>"""
        result = runner.live_price_check(catalog, pricing)
        self.assertTrue(result["limited_time_free"])

    def test_nonzero_or_missing_price_fails_closed(self):
        catalog = json.dumps({"data": [{"id": "step-5-preview-free", "owned_by": "opencode"}]}).encode()
        pricing = b"""<table><tr><td>Step 5 Preview Free</td><td>Free</td><td>$0.01</td><td>Free</td><td>-</td></tr></table>
        Step 5 Preview Free is free on OpenCode for a limited time."""
        with self.assertRaisesRegex(RuntimeError, "no longer explicitly zero"):
            runner.live_price_check(catalog, pricing)

    def test_missing_model_fails_closed(self):
        catalog = json.dumps({"data": [{"id": "another-free-model", "owned_by": "opencode"}]}).encode()
        with self.assertRaisesRegex(RuntimeError, "absent or ambiguous"):
            runner.live_price_check(catalog, b"")

    def test_structured_service_error_cannot_be_a_success_even_with_exit_zero(self):
        event = json.dumps({
            "type": "error",
            "sessionID": "session-test",
            "error": {"type": "provider.auth", "message": "free tier denied", "status": 403},
        })
        success, errors = runner.completion_success(0, event + "\nSTEP5_OK", "")
        self.assertFalse(success)
        self.assertTrue(errors)

    def test_nested_error_payload_in_stderr_cannot_be_a_success(self):
        error = json.dumps({"type": "error", "error": {"type": "FreeTierError", "status": 403}})
        success, errors = runner.completion_success(0, "STEP5_OK", error)
        self.assertFalse(success)
        self.assertTrue(errors)

    def test_success_requires_zero_exit_exact_probe_marker_and_no_errors(self):
        self.assertEqual(runner.completion_success(0, "STEP5_OK\n", ""), (True, []))
        self.assertFalse(runner.completion_success(0, "other", "")[0])
        self.assertFalse(runner.completion_success(1, "STEP5_OK", "")[0])

    def test_config_adds_model_without_replacing_builtin_provider_runtime(self):
        config = json.loads((ROOT / "tools/agents/opencode-v2-step5-smoke.json").read_text())
        provider = config["providers"]["opencode"]
        self.assertEqual(config["model"], "opencode/step-5-preview-free")
        self.assertEqual(provider["models"]["step-5-preview-free"]["modelID"], "step-5-preview-free")
        self.assertNotIn("package", provider)
        self.assertNotIn("settings", provider)


if __name__ == "__main__":
    unittest.main()
