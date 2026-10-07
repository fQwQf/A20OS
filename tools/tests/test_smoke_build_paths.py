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
        common = ("ARCH=riscv64", "ABI=linux")
        default = self.resolve_dir(*common)
        self.assertTrue(default.endswith("riscv64-qemu-virt-riscv64-linux-dev-preempt"),
                        default)

        preempt_off = self.resolve_dir(*common, "CONFIG_KERNEL_PREEMPT=0")
        self.assertTrue(preempt_off.endswith("riscv64-qemu-virt-riscv64-linux-dev"),
                        preempt_off)

        smp = self.resolve_dir(*common, "NR_CPUS=2", "ALLOW_UNVERIFIED_SMP=1")
        self.assertTrue(smp.endswith("riscv64-qemu-virt-riscv64-linux-dev-smp2-preempt"),
                        smp)

        embedded = self.resolve_dir("ARCH=aarch64", "ABI=linux",
                                    "DRIVER_DEPLOYMENT=embedded")
        self.assertTrue(embedded.endswith("aarch64-qemu-virt-aarch64-linux-dev-embedded-preempt"),
                        embedded)

        override = self.resolve_dir(*common, "CONFIG_SLAB_DEBUG=1")
        self.assertTrue(override.endswith("riscv64-qemu-virt-riscv64-linux-dev-preempt-slabdbg"),
                        override)

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
        expected = build_dir.replace("-dev", "-dev-ramfs-user", 1)
        self.assertEqual(ramfs_dir, expected)


if __name__ == "__main__":
    unittest.main()
