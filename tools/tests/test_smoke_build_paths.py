"""Smoke argv paths must follow the exact build variables for each case."""

import sys
import subprocess
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import smoke  # noqa: E402
from a20_derive import derive_make_vars  # noqa: E402
from a20_instance import parse_instance  # noqa: E402


class SmokeBuildPathTests(unittest.TestCase):
    def resolve_dir(self, *variables):
        return smoke.resolve_build_dir({
            "name": "path-test",
            "build": {"vars": list(variables), "target": "dev-build"},
        })

    def test_make_resolves_default_preempt_smp_embedded_and_overrides(self):
        """Every override must reach make and select a distinct output directory.

        The directory *name* is not asserted.  A fully integrated Rust profile
        makes BUILD_VARIANT long enough that the Makefile hashes it (see the
        BUILD_VARIANT_FULL comment there), so the descriptive spelling is no
        longer a contract.  Distinctness is the contract that matters: an
        override that stops being forwarded collapses two configurations onto
        one directory and silently reuses the other one's objects.
        """
        common = ("ARCH=riscv64", "ABI=linux")
        variants = {
            "default": self.resolve_dir(*common),
            "preempt_off": self.resolve_dir(*common, "CONFIG_KERNEL_PREEMPT=0"),
            "smp2": self.resolve_dir(*common, "NR_CPUS=2", "ALLOW_UNVERIFIED_SMP=1"),
            "embedded": self.resolve_dir("ARCH=aarch64", "ABI=linux",
                                         "DRIVER_DEPLOYMENT=embedded"),
            "slabdbg": self.resolve_dir(*common, "CONFIG_SLAB_DEBUG=1"),
        }
        self.assertTrue(variants["default"].startswith(
            ".kernel-build/riscv64-qemu-virt-riscv64-"), variants["default"])
        self.assertTrue(variants["embedded"].startswith(
            ".kernel-build/aarch64-qemu-virt-aarch64-"), variants["embedded"])
        seen: dict[str, str] = {}
        for name, path in variants.items():
            self.assertNotIn(path, seen,
                             f"{name} resolves to the same directory as {seen.get(path)}")
            seen[path] = name

    def test_rebases_kernel_and_sibling_images_but_keeps_other_builds(self):
        old = ".kernel-build/riscv64-qemu-virt-riscv64-linux-dev"
        other = ".kernel-build/riscv64-qemu-virt-riscv64-both-dev"
        case = {"argv": ["qemu-system-riscv64", "-kernel", f"{old}/kernel.elf",
                         "-drive", f"file={old}/fat32.img,if=none,id=x0",
                         "-drive", f"file={other}/lfs.img,if=none,id=x1",
                         "-drive", "file=.kernel-build/nvme-scratch.img,id=x2"]}

        actual = smoke.resolve_build_paths(case, ".kernel-build/build-dev-preempt")
        self.assertEqual(actual["argv"][2], ".kernel-build/build-dev-preempt/kernel.elf")
        self.assertEqual(actual["argv"][4],
                         "file=.kernel-build/build-dev-preempt/fat32.img,if=none,id=x0")
        self.assertEqual(actual["argv"][6], f"file={other}/lfs.img,if=none,id=x1")
        self.assertEqual(actual["argv"][8], "file=.kernel-build/nvme-scratch.img,id=x2")

    def test_build_dir_placeholder_remains_supported(self):
        case = {"argv": ["-kernel", "@BUILD_DIR@/kernel.elf",
                         "-drive", "file=@BUILD_DIR@/fat32.img"]}
        actual = smoke.resolve_build_paths(case, ".kernel-build/placeholder-target")
        self.assertEqual(actual["argv"], ["-kernel", ".kernel-build/placeholder-target/kernel.elf",
                                          "-drive", "file=.kernel-build/placeholder-target/fat32.img"])

    def test_make_qemu_paths_and_a20_instance_use_the_same_build_dir(self):
        inst = parse_instance(Path("instances/check-fex-precond-aarch64.toml"))
        variables = derive_make_vars(inst)
        build_dir = self.resolve_dir(*variables)
        qemu = subprocess.run(
            ["make", "--no-print-directory", *variables, "_qemu_argv"],
            check=True, capture_output=True, text=True,
        ).stdout.splitlines()[-1]
        self.assertIn(f"-kernel {build_dir}/kernel.elf", qemu)
        self.assertIn(f"file={build_dir}/fat32.img", qemu)

        # The same resolver is used by native hyp smokes: their guest payload
        # lives in the rootfs image, while the outer kernel and image must be
        # from the case's preemption/SMP-specific build directory.
        hyp = smoke.resolve_case_build_paths(smoke.CASES["smoke-hyp-vcpu"])
        kernel = hyp["argv"][hyp["argv"].index("-kernel") + 1]
        disk = next(a for a in hyp["argv"] if a.startswith("file=") and "fat32.img" in a)
        expected = smoke.resolve_build_dir(smoke.CASES["smoke-hyp-vcpu"])
        self.assertEqual(kernel, f"{expected}/kernel.elf")
        self.assertIn(f"file={expected}/fat32.img", disk)

    def test_ramfs_guest_kernel_path_inherits_preempt_variant(self):
        variables = ("ARCH=riscv64", "BOARD=qemu-virt-riscv64", "ABI=linux")
        build_dir = self.resolve_dir(*variables)
        make_db = subprocess.run(
            ["make", "--no-print-directory", *variables, "-pn", "print-build-dir"],
            check=True, capture_output=True, text=True,
        ).stdout
        ramfs_dir = next(
            line.split(":=", 1)[1].strip()
            for line in make_db.splitlines()
            if line.startswith("GUEST_KERNEL_RAMFS_DIR :=")
        )
        expected = build_dir + "-ramfs-user"
        self.assertEqual(ramfs_dir, expected)

    def test_ramfs_companion_is_distinct_for_default_and_custom_build_dirs(self):
        base = ("ARCH=riscv64", "BOARD=qemu-virt-riscv64", "ABI=linux", "BRINGUP=0",
                "CONFIG_KERNEL_PREEMPT=0")
        # Ask the Makefile for the default directory; the descriptive spelling
        # is not stable once BUILD_VARIANT is long enough to be hashed.
        default = smoke.resolve_build_dir(
            {"name": "ramfs-companion", "build": {"vars": list(base)}})
        cases = [
            (base, default),
            (base + ("BUILD_DIR=/tmp/a20-custom-build",), "/tmp/a20-custom-build"),
        ]
        resolved = []
        for variables, build_dir in cases:
            with self.subTest(build_dir=build_dir):
                ramfs_dir = build_dir + "-ramfs-user"
                output = subprocess.run(
                    ["make", "--no-print-directory", *variables, "-n",
                     f"{ramfs_dir}/.ramfs-user-elf.stamp"],
                    check=True, capture_output=True, text=True,
                ).stdout
                self.assertIn(
                    f"RAMFS_USER=1 BUILD_DIR={ramfs_dir} kernel-only", output
                )
                resolved.append(ramfs_dir)
        self.assertNotEqual(resolved[0], resolved[1],
                            "the default and custom build dirs share a ramfs companion")

    def test_netopt_lwip_uses_resolved_smp4_lane4_image_and_real_lwip_mode(self):
        case = smoke.CASES["smoke-netopt-lwip"]
        self.assertEqual(case["build"]["vars"], [
            "ARCH=riscv64", "ABI=linux", "BRINGUP=0", "NR_CPUS=4", "NET_LANES=4",
            "OPT=-O3 -DCONFIG_NET_PCB_SANE=1",
        ])
        self.assertEqual(case["stdin"]["lines"],
                         ["netopt_test --lwip-listener", "poweroff"])
        self.assertEqual(case["expect"], ["NETOPT: PASS"])
        self.assertIn("a20.tcpmode=lwip", case["argv"][-1])
        resolved = smoke.resolve_case_build_paths(case)
        build_dir = smoke.resolve_build_dir(case)
        kernel = resolved["argv"][resolved["argv"].index("-kernel") + 1]
        disk = next(arg for arg in resolved["argv"] if arg.startswith("file=")
                    and "fat32.img" in arg)
        self.assertEqual(kernel, f"{build_dir}/kernel.elf")
        self.assertEqual(disk, f"file={build_dir}/fat32.img,if=none,format=raw,id=x0")
        self.assertEqual(resolved["argv"][resolved["argv"].index("-smp") + 1], "4")


if __name__ == "__main__":
    unittest.main()
