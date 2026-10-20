#!/usr/bin/env python3
"""Regression checks for the opt-in freestanding Rust user peer rules."""

from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]


def run_build_script() -> Path:
    """The shared build-lock wrapper, located through git.

    In a linked worktree ROOT/.git is a file, so the wrapper cannot be found by
    path arithmetic; ask git for the common dir, which is what the wrapper
    itself does for the lock files.  Without this the whole module errored out
    in every worktree -- the normal way this repository is worked on -- with a
    NotADirectoryError that looks nothing like a make-rule regression.
    """
    common = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute",
         "--git-common-dir"],
        check=True, capture_output=True, text=True).stdout.strip()
    return Path(common) / "parallel-rewrite" / "run-build.sh"


def make_dry_run(*args):
    return subprocess.run([str(run_build_script()),
                           "build-1", "make", "-n",
                           "BUILD_DIR=<build-dir>/isolated/rust-user-make-tests/build",
                           "USER_BUILD_DIR=<build-dir>/isolated/rust-user-make-tests/user",
                           *args], cwd=ROOT, text=True,
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
        self.assertNotIn("rust_idl_adversary.rs", output)
        self.assertNotIn("rust-idl-peer-ack-rv", output)

    def test_ack_adversarial_profile_is_explicit_and_builds_test_peers(self):
        result = make_dry_run("ARCH=riscv64", "NATIVE_RUST_PEER=1",
                              "native-rust-idl-ack-test")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout + result.stderr
        self.assertIn("--cfg idl_ack_test", output)
        self.assertIn("user/svc/rust_idl_adversary.rs", output)
        self.assertIn("rust-idl-peer-ack-rv", output)
        self.assertIn("rust-idl-adversary-rv", output)

    def test_opt_in_peer_rejects_unsupported_architecture(self):
        result = make_dry_run("ARCH=loongarch64", "NATIVE_RUST_PEER=1", "native-programs")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires ARCH=", result.stderr)

    def test_opt_in_x86_64_peer_uses_native_target_and_sdk_link(self):
        result = make_dry_run("ARCH=x86_64", "NATIVE_RUST_PEER=1",
                              "native-rust-idl-peer-x86_64")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout + result.stderr
        self.assertIn("--target x86_64-unknown-none", output)
        self.assertIn("user/svc/rust_idl_peer.rs", output)
        self.assertIn("rust-idl-peer-x86_64", output)
        self.assertIn("crt0_x86_64.S", output)

    def test_x86_64_ack_profile_builds_only_explicit_adversarial_peers(self):
        result = make_dry_run("ARCH=x86_64", "native-rust-idl-ack-test-x86_64")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout + result.stderr
        self.assertIn("--cfg idl_ack_test", output)
        self.assertIn("user/svc/rust_idl_adversary.rs", output)
        self.assertIn("rust-idl-peer-ack-x86_64", output)
        self.assertIn("rust-idl-adversary-x86_64", output)

    def test_x86_64_rust_service_uses_same_native_crt(self):
        result = make_dry_run("ARCH=x86_64", "native-rust-idl-service-x86_64")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout + result.stderr
        self.assertIn("user/svc/rust_idl_echod.rs", output)
        self.assertIn("rust-idl-echod-x86_64", output)
        self.assertIn("native-rust-idl-service-x86_64", output)

    def test_opt_in_rv32_peer_uses_ilp32d_core_and_target(self):
        result = make_dry_run("ARCH=riscv32", "NATIVE_RUST_PEER=1",
                              "native-rust-idl-peer-arch")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout + result.stderr
        self.assertIn("riscv32imafdc-a20.json", output)
        self.assertIn("-mabi=ilp32d", output)
        self.assertIn("-Z build-std=core", output)
        self.assertIn("rust-idl-peer-rv32", output)

    def test_rv32_convenience_targets_are_explicit(self):
        for target in ("native-rust-idl-peer-rv32",
                       "native-rust-idl-adversary-rv32",
                       "native-rust-idl-ack-test-rv32"):
            result = make_dry_run(target)
            self.assertEqual(result.returncode, 0, (target, result.stderr))
            self.assertIn("ARCH=riscv32", result.stdout + result.stderr)
        service = make_dry_run("native-rust-idl-service-rv32")
        self.assertEqual(service.returncode, 0, service.stderr)
        self.assertIn("ARCH=riscv32", service.stdout + service.stderr)

    def test_rust_svcmgr_builds_for_each_installed_native_target(self):
        for arch, tag in (("riscv64", "rv"), ("riscv32", "rv32"),
                          ("aarch64", "aarch64"), ("x86_64", "x86_64")):
            result = make_dry_run(f"ARCH={arch}", "NATIVE_RUST_SVCMGR=1",
                                  "native-rust-svcmgr")
            self.assertEqual(result.returncode, 0, (arch, result.stderr))
            output = result.stdout + result.stderr
            self.assertIn("user/svc/svcmgr.rs", output)
            self.assertIn(f"svcmgr-rust-{tag}", output)

    def test_rust_rtcd_is_limited_to_goldfish_targets(self):
        result = make_dry_run("ARCH=aarch64", "NATIVE_RUST_RTCD=1",
                              "native-rust-rtcd")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("rtcd-rust-aarch64.a20drv", result.stdout + result.stderr)
        for arch in ("x86_64", "riscv32"):
            rejected = make_dry_run(f"ARCH={arch}", "NATIVE_RUST_RTCD=1",
                                    "native-rust-rtcd")
            self.assertNotEqual(rejected.returncode, 0, arch)
            self.assertIn("requires ARCH=riscv64 or aarch64 Goldfish RTC",
                          rejected.stderr)

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
