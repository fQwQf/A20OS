#!/usr/bin/env python3
"""Build the independent GPL rtw89 service against A20OS's static musl."""
import argparse
import os
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PIN = "90b6cfc5a69c271688fad188857cfaeb58053dc5"
FW_HASH = "6ff6aeb56bb5b85cba333b755f8cc1dbfe9ff6e5b16e39f6ba01ce7f1eaa7714"
FW_COMMIT = "d947e4e8e314e9254a1242dc1a5d9cede2cce33d"


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--musl-build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    source = ROOT / "user/external/rtw89"
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                       text=True).strip()
    if revision != PIN:
        parser.error(f"rtw89 submodule must be at {PIN}, found {revision}")
    if subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"]).strip():
        parser.error("rtw89 submodule has local modifications")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    firmware = ROOT / "build/downloads/rtw8852b_fw-2.bin"
    firmware.parent.mkdir(parents=True, exist_ok=True)
    if not firmware.exists():
        temporary = firmware.with_suffix(".download")
        url = ("https://git.kernel.org/pub/scm/linux/kernel/git/firmware/"
               f"linux-firmware.git/plain/rtw89/{firmware.name}?id={FW_COMMIT}")
        subprocess.run(["curl", "-fL", "--retry", "2", "--max-time", "120",
                        url, "-o", str(temporary)], check=True)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != FW_HASH:
            parser.error("downloaded firmware checksum mismatch")
        temporary.replace(firmware)
    if hashlib.sha256(firmware.read_bytes()).hexdigest() != FW_HASH:
        parser.error("rtw8852b firmware checksum mismatch")
    musl = ROOT / "user/external/musl"
    base = ["-O2", "-g", "-fno-pie", "-fno-strict-aliasing", "-fwrapv",
            "-fno-delete-null-pointer-checks", "-ffunction-sections", "-fdata-sections",
            "-nostdinc", "-pthread", "-std=gnu11"]
    for folder in [args.musl_build / "obj/include", musl / "include",
                   musl / "arch/x86_64", musl / "arch/generic"]:
        base += ["-isystem", str(folder.resolve())]
    compiler = shlex.split(args.cc)
    compiler_headers = subprocess.check_output(
        [*compiler, "-print-file-name=include"], text=True).strip()
    base += ["-idirafter", compiler_headers]
    compat = source / "src/compat"
    wireless = source / "src/compat_rtw89"
    port = ROOT / "user/wifi"
    driver = ["-DRTW89_MACOS=1", "-DRTW88_MACOS=1", "-D__KERNEL__",
              "-DCONFIG_RTW89_DEBUGMSG=1", "-Wno-address-of-packed-member"]
    for folder in [port / "compat", wireless, source / "third_party/linux-include",
                   source / "third_party/linux-include/uapi", compat, compat / "linux",
                   source / "third_party/rtw89"]:
        driver += ["-I" + str(folder)]
    driver += ["-include", str(wireless / "rtw89_compat.h")]
    core = ("mac80211 mac mac_be phy phy_be fw cam efuse efuse_be regd sar coex "
            "ps chan ser acpi util pci pci_be rtw8852b_common rtw8852b rtw8852b_table "
            "rtw8852b_rfk rtw8852b_rfk_table rtw8852be").split()
    units = [(source / f"third_party/rtw89/{name}.c", driver) for name in core]
    units += [(compat / f"{name}.c", driver)
              for name in ("rtw88_compat", "rtw88_thread_call")]
    names = ("rtw89_compat rtw89_cfg80211 rtw89_cfg80211_bitrate rtw89_mac80211 "
             "rtw89_mlme rtw89_data rtw89_awdl rtw89_core_wrap rtw89_debug_shim").split()
    units += [(wireless / f"{name}.c", driver) for name in names]
    original = (wireless / "rtw89_glue.c").read_text()
    start = original.index("#define GLUE_PCI_DRIVERS(x)")
    end = original.index("#define GLUE_DECLARE", start)
    glue = out / "rtw89_glue.c"
    glue.write_text(original[:start] +
                    "#define GLUE_PCI_DRIVERS(x) x(rtw89_8852be_driver)\n" + original[end:])
    units += [(glue, driver)]
    hostap = source / "third_party/hostap"
    mbedtls = source / "third_party/mbedtls"
    sae = ["-DMBEDTLS_CONFIG_FILE=\"rtw89_mbedtls_config.h\"",
           "-include", str(port / "compat/runtime_decls.h")]
    for folder in [wireless / "hostap", hostap / "src", hostap / "src/utils",
                   hostap / "src/common", mbedtls / "include", mbedtls / "library",
                   wireless]:
        sae += ["-I" + str(folder)]
    units += [(hostap / f"src/common/{name}.c", sae) for name in ("sae", "dragonfly")]
    units += [(mbedtls / f"library/{name}.c", sae) for name in
              ("bignum", "bignum_core", "ecp", "ecp_curves", "constant_time", "platform_util")]
    units += [(wireless / f"{name}.c", sae) for name in
              ("rtw89_wpabuf", "rtw89_hostap", "rtw89_sae", "rtw89_crypto")]
    units += [(source / "tools/hosttest/selftest.c", driver)]
    units += [(port / name, ["-I" + str(wireless), "-I" + str(ROOT / "kernel/include")])
              for name in ("runtime.c", "firmware.c", "rtw89d.c")]
    assembly = out / "firmware_blob.S"
    assembly.write_text('.section .rodata\n.global a20_rtw89_firmware_begin\n'
                        'a20_rtw89_firmware_begin:\n.incbin "' + str(firmware) +
                        '"\n.global a20_rtw89_firmware_end\na20_rtw89_firmware_end:\n'
                        '.section .note.GNU-stack,"",@progbits\n')
    units += [(assembly, [])]
    commands = []

    def compile_unit(unit):
        path, extra = unit
        target = out / (path.stem + ".o")
        command = [*compiler, *base, *extra, "-c", str(path), "-o", str(target)]
        result = subprocess.run(command, capture_output=True, text=True)
        (out / (path.stem + ".log")).write_text(result.stdout + result.stderr)
        commands.append(command)
        if result.returncode:
            raise RuntimeError(f"{path.name} failed. See {out / (path.stem + '.log')}")
        return target

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        objects = list(pool.map(compile_unit, units))
    crt = args.musl_build.resolve() / "lib"
    binary = out / "rtw89d"
    command = [*compiler, "-static", "-no-pie", "-nostdlib", "-Wl,--gc-sections",
               str(crt / "crt1.o"), str(crt / "crti.o"), *map(str, objects),
               str(crt / "libc.a"), *( [str(Path(os.environ.get("LLVM_RT_ROOT", ROOT / "build/toolchains/compiler-rt")) / "x86_64-user/builtins.a")] if os.environ.get("TOOLCHAIN") == "llvm" else ["-lgcc"] ), str(crt / "crtn.o"), "-o", str(binary)]
    subprocess.run(command, check=True)
    (out / "build.json").write_text(json.dumps({
        "upstream_commit": PIN, "firmware_sha256": FW_HASH,
        "firmware_commit": FW_COMMIT, "commands": commands, "link": command,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    }, indent=2) + "\n")
    print(binary)


if __name__ == "__main__":
    main()
