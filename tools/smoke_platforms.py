"""Parameterize shared smoke assertions without copying their case tables."""

from copy import deepcopy
import re

ARCHES = ("riscv64", "aarch64")
# These require board-private MMIO windows and IRQ allowlists.
RISCV_ONLY = {
    "smoke-native-rtcd": "goldfish RTC MMIO/IRQ",
    "smoke-native-registry": "goldfish RTC service restart",
    "smoke-native-ubd": "reserved RISC-V virtio-blk MMIO/IRQ",
}


def case_for_arch(name: str, case: dict, arch: str) -> dict:
    if arch not in ARCHES:
        raise ValueError(f"unsupported shared smoke architecture: {arch}")
    if not (name.startswith("smoke-native-") or
            name in {"smoke-pty-stress", "smoke-timeout-test", "smoke-swap"}):
        raise ValueError(f"{name} has no shared platform definition")
    if name in RISCV_ONLY and arch != "riscv64":
        raise ValueError(f"{name} requires {RISCV_ONLY[name]}")
    result = deepcopy(case)
    if name.startswith("smoke-native-"):
        # Wait for the shell instead of losing input during a slow TCG boot.
        if result.get("stdin", {}).get("kind") == "pipe":
            result["stdin"] = {"kind": "sendline", "expect": "# ",
                               "lines": result["stdin"]["lines"]}
        seconds = int(result["timeout"].removesuffix("s"))
        result["timeout"] = f"{max(60, seconds)}s"
    if arch == "riscv64":
        return result

    def rebase(value):
        if isinstance(value, str):
            value = value.replace("riscv64", arch)
            return re.sub(r"(/bin/[\w-]+)-rv(?=[.\s]|$)",
                          rf"\1-{arch}", value)
        if isinstance(value, list):
            return [rebase(item) for item in value]
        if isinstance(value, dict):
            return {key: rebase(item) for key, item in value.items()}
        return value

    result = rebase(result)
    build_vars = result["build"]["vars"]
    build_vars[:] = [v for v in build_vars if not v.startswith("BOARD=")]
    build_vars.append(f"BOARD=qemu-virt-{arch}")
    argv = result["argv"]
    if "-bios" in argv:
        pos = argv.index("-bios")
        del argv[pos:pos + 2]
    pos = argv.index("-machine") + 2
    argv[pos:pos] = ["-cpu", "cortex-a57"]
    return result
