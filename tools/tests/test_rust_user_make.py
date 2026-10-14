#!/usr/bin/env python3
"""Regression checks for the opt-in freestanding Rust user peer rules."""

from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]


def make_dry_run(*args):
    return subprocess.run(["make", "-n", *args], cwd=ROOT, text=True,
                         capture_output=True)


class RustUserMakeTests(unittest.TestCase):
    def test_c_only_clean_allows_o0(self):
        result = make_dry_run("ARCH=x86_64", "OPT=-O0", "clean")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_non_rv64_native_programs_do_not_build_rust_peer(self):
        result = make_dry_run("ARCH=x86_64", "native-programs")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("rust_idl_peer", result.stdout + result.stderr)
        self.assertNotIn("rust-idl-peer", result.stdout + result.stderr)

    def test_opt_in_rv64_native_programs_include_peer_compile_and_link(self):
        result = make_dry_run("ARCH=riscv64", "NATIVE_RUST_PEER=1", "native-programs")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout + result.stderr
        self.assertIn("rustc", output)
        self.assertIn("user/svc/rust_idl_peer.rs", output)
        self.assertIn("rust-idl-peer-rv", output)

    def test_opt_in_peer_rejects_unsupported_architecture(self):
        result = make_dry_run("ARCH=x86_64", "NATIVE_RUST_PEER=1", "native-programs")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("currently supported only for ARCH=riscv64", result.stderr)

    def test_opt_in_peer_rejects_o0_instead_of_silently_mapping_it(self):
        result = make_dry_run("ARCH=riscv64", "NATIVE_RUST_PEER=1",
                              "USER_OPT=-O0", "native-programs")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not support USER_OPT='-O0'", result.stderr)

    def test_rust_service_is_not_in_default_native_programs(self):
        result = make_dry_run("ARCH=riscv64", "native-programs")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout + result.stderr
        self.assertNotIn("rust_idl_echod.rs", output)
        self.assertNotIn("native-rust-idl-service-rv", output)

    def test_rust_service_opt_in_builds_rust_service_and_c_client(self):
        result = make_dry_run("ARCH=riscv64", "NATIVE_RUST_IDL_SERVICE=1",
                              "native-programs")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout + result.stderr
        self.assertIn("user/svc/rust_idl_echod.rs", output)
        self.assertIn("rust-idl-echod-rv", output)
        self.assertIn("test_native_rust_idl_service.c", output)
        self.assertIn("native-rust-idl-service-rv", output)

    def test_direct_arch_target_requires_selector(self):
        result = make_dry_run("ARCH=riscv64", "native-rust-idl-peer-arch")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires NATIVE_RUST_PEER=1", result.stderr)


if __name__ == "__main__":
    unittest.main()
