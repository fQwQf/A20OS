from __future__ import annotations

import json
import unittest

from tools.netnat_result import MARKER, ResultError, validate_log


PAYLOAD = {
    "schema": 1,
    "case": "nat-rewrite/dnat-tcp-echo-v1",
    "contract": {
        "completed": 16,
        "errors": 0,
        "payload_hash_fnv1a32": 3919010405,
        "forward_port": 18081,
        "listen_port": 18082,
        "nat_action": "dnat",
    },
    "metrics": {
        "elapsed_ns": 20000000,
        "ns_per_transaction": 1250000.0,
        "transactions_per_second": 800.0,
    },
}


def valid_log() -> str:
    return "\n".join((
        "NETNAT_TEST: PASS dnat 18081 -> 10.0.2.15:18082 completed=16",
        MARKER + " " + json.dumps(PAYLOAD),
        "System is going down for power-off NOW.",
        "[SD] POWER_OFF entry",
        "[SD] audit errors=0[MM-ASM] pt_pages=6",
    ))


class NetnatResultTests(unittest.TestCase):
    def test_valid_fixed_fixture_passes(self) -> None:
        result = validate_log(valid_log(), qemu_status=0, probe_status=0,
                              expected_transactions=16)
        self.assertEqual(result, PAYLOAD)

    def test_pass_marker_does_not_override_qemu_timeout(self) -> None:
        with self.assertRaisesRegex(ResultError, "QEMU exited"):
            validate_log(valid_log(), qemu_status=124, probe_status=0)

    def test_panic_after_pass_is_rejected(self) -> None:
        with self.assertRaisesRegex(ResultError, "fatal marker"):
            validate_log(valid_log() + "\nKERNEL PANIC after pass\n",
                         qemu_status=0, probe_status=0)

    def test_missing_marker_is_rejected(self) -> None:
        log = "\n".join(line for line in valid_log().splitlines()
                         if MARKER not in line)
        with self.assertRaisesRegex(ResultError, "exactly one"):
            validate_log(log, qemu_status=0, probe_status=0)

    def test_duplicate_marker_is_rejected(self) -> None:
        with self.assertRaisesRegex(ResultError, "exactly one"):
            validate_log(valid_log() + "\n" + MARKER + " " + json.dumps(PAYLOAD),
                         qemu_status=0, probe_status=0)

    def test_wrong_fixture_hash_is_rejected(self) -> None:
        payload = json.loads(json.dumps(PAYLOAD))
        payload["contract"]["payload_hash_fnv1a32"] += 1
        log = valid_log().replace(json.dumps(PAYLOAD), json.dumps(payload))
        with self.assertRaisesRegex(ResultError, "fixed 16-transaction fixture"):
            validate_log(log, qemu_status=0, probe_status=0)

    def test_missing_shutdown_audit_is_rejected(self) -> None:
        log = "\n".join(line for line in valid_log().splitlines()
                         if "audit errors=" not in line)
        with self.assertRaisesRegex(ResultError, "shutdown audit"):
            validate_log(log, qemu_status=0, probe_status=0)

    def test_probe_failure_is_rejected(self) -> None:
        with self.assertRaisesRegex(ResultError, "host probe exited"):
            validate_log(valid_log(), qemu_status=0, probe_status=1)

    def test_transaction_override_must_match_registered_fixture(self) -> None:
        with self.assertRaisesRegex(ResultError, "no registered NAT echo fixture"):
            validate_log(valid_log(), qemu_status=0, probe_status=0,
                         expected_transactions=15)


if __name__ == "__main__":
    unittest.main()
