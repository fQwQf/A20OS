"""Run the vendored full interpreter and production fw_node adapter on real AML.
Host services are single-thread fixtures, not tests of the kernel OSL or hardware.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

def package(op, body):
    # Standard AML PkgLength includes its own bytes, excluding the opcode.
    size = len(body) + 1
    if size < 64:
        return op + bytes([size]) + body
    size = len(body) + 2
    return op + bytes([0x40 | (size & 15), size >> 4]) + body

def string(value):
    return b"\x0d" + value.encode() + b"\0"

def name(n, value):
    return b"\x08" + n.encode() + value

def method(n, value, args=0):
    return package(b"\x14", n.encode() + bytes([args, 0xa4]) + value)

def fixture():
    # _HID and _STA are METHODS, not literal Name constants. _STA executes Or.
    resource = b"\x86\x09\x00\x01" + struct.pack("<II", 0x10000000, 0x1000)
    resource += b"\x89\x06\x00\x0d\x01" + struct.pack("<I", 21) + b"\x79\x00"
    crs = package(b"\x11", b"\x0a" + bytes([len(resource)]) + resource)
    cid = package(b"\x12", b"\x02" + string("A20C0001") + string("A20C0002"))
    dev = package(b"\x5b\x82", b"DEV0" + method("_HID", string("A20T0001")) +
                  name("_CID", cid) + method("_STA", b"\x7d\x0a\x0c\x0a\x03\x00") +
                  name("_ADR", b"\x0c\x00\x00\x01\x00") + name("_CRS", crs))
    route = package(b"\x12", b"\x04\x0c\xff\xff\x00\x00\x00\x00\x0a\x15")
    prt = package(b"\x12", b"\x01" + route)
    pci = package(b"\x5b\x82", b"PCI0" + name("_HID", string("PNP0A08")) +
                  name("_SEG", b"\x00") + name("_BBN", b"\x00") + method("_PRT", prt))
    tpm = package(b"\x5b\x82", b"TPM0" + method("_HID", string("MSFT0101")))
    child = package(b"\x5b\x82", b"CHD0" + method("_HID", string("A20C0003")))
    parent = package(b"\x5b\x82", b"PAR0" + method("_HID", string("A20P0001")) +
                     method("_STA", b"\x0a\x08") + child)
    i2c = bytes([2, 0, 1, 2, 0, 0, 1]) + struct.pack("<HIH", 6, 400000, 0x2c)
    i2c += b"\\_SB.I2CA\0"
    # GpioInt (Level, ActiveLow, ExclusiveAndWake, PullUp), pin 9.
    gpio_int = bytes.fromhex("8c 20 00 01 00 01 00 12 00 01 00 00 00 00 "
                            "17 00 00 19 00 23 00 00 00 09 00") + b"\\_SB.GPIO\0"
    i2c = b"\x8e" + struct.pack("<H", len(i2c)) + i2c + gpio_int + b"\x79\0"
    i2c_crs = package(b"\x11", bytes([0x0a, len(i2c)]) + i2c)
    touchpad = package(b"\x5b\x82", b"TPDD" + name("_HID", string("SYNA2BA6")) +
                       name("_CID", string("PNP0C50")) + name("_CRS", i2c_crs) +
                       method("_DSM", b"\x0a\x20", args=4))
    controller = package(b"\x5b\x82", b"I2CA" + name("_HID", string("AMDI0010")) + touchpad)
    gpio = package(b"\x5b\x82", b"GPIO" + name("_HID", string("AMDI0030")))
    aml = package(b"\x10", b"\\_SB_" + dev + pci + tpm + parent + controller + gpio)
    ram = bytearray(65536)
    def table(pa, sig, body):
        header = bytearray(36)
        header[:4] = sig.encode();struct.pack_into("<I", header, 4, 36 + len(body))
        header[8] = 6;header[10:16] = b"A20OS ";header[16:24] = b"HOSTTEST"
        data = header + body;data[9] = -sum(data) & 255;ram[pa:pa+len(data)] = data
    fadt = bytearray(276 - 36)
    struct.pack_into("<I", fadt, 40 - 36, 0x4000)
    struct.pack_into("<I", fadt, 112 - 36, 1 << 20)  # HW_REDUCED_ACPI: no host I/O.
    struct.pack_into("<Q", fadt, 140 - 36, 0x4000)
    table(0x2000, "FACP", fadt);table(0x4000, "DSDT", aml)
    tpm2 = bytearray(52 - 36)
    struct.pack_into("<I", tpm2, 48 - 36, 6)
    table(0x6000, "TPM2", tpm2)
    table(0x1000, "XSDT", struct.pack("<QQ", 0x2000, 0x6000))
    rsdp = bytearray(36);rsdp[:8] = b"RSD PTR ";rsdp[15] = 2
    struct.pack_into("<I", rsdp, 20, 36);struct.pack_into("<Q", rsdp, 24, 0x1000)
    rsdp[8] = -sum(rsdp[:20]) & 255;rsdp[32] = -sum(rsdp) & 255
    ram[0x100:0x124] = rsdp
    return ram

@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class UacpiNamespaceTests(unittest.TestCase):
    def test_real_aml_and_production_adapter(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder);binary = base / "uacpi-test"
            (base / "firmware.bin").write_bytes(fixture())
            sources = list((ROOT / "kernel/external/uacpi/source").glob("*.c"))
            sources += [ROOT / "kernel/firmware/acpi" / n for n in
                        ("tables.c", "topology.c", "uacpi_host.c")]
            sources += [ROOT / "tools/tests" / n for n in
                        ("uacpi_host_test_osl.c", "test_uacpi_namespace.c")]
            subprocess.run(["cc", "-std=gnu99", "-O1", "-g", "-ffreestanding", "-DCONFIG_UACPI", "-DCONFIG_NR_CPUS=1",
                            "-fsanitize=undefined", "-fno-sanitize-recover=all",
                            "-I" + str(ROOT / "kernel/include"),
                            "-I" + str(ROOT / "kernel/external/uacpi/include"),
                            "-I" + str(ROOT / "kernel/arch/x86_64/include"),
                            *map(str, sources), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), str(base / "firmware.bin")],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("real AML namespace/HID/CID/STA/CRS/PRT PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
