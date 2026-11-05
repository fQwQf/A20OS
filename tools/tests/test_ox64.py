"""Ox64 D0 hardware contracts and media formats, without board/serial writes."""
from pathlib import Path
import ctypes
from contextlib import redirect_stdout
import io
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "boards"))
import ox64
from raspberrypi4_fixture import Tree


def tree():
    t = Tree()
    t.begin("")
    t.text("compatible", "pine64,ox64\0bflb,bl808")
    t.cells("#address-cells", 1)
    t.cells("#size-cells", 1)
    t.begin("memory@50000000")
    t.text("device_type", "memory")
    t.cells("reg", 0x50000000, 0x4000000)
    t.end()
    t.end()
    return t.finish([])


class Ox64Contracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="a20-ox64-test-")
        p = Path(cls.temp.name)
        c = p / "wrapper.c"
        c.write_text('''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static volatile uint32_t regs[0x100/4];
#define UART0_BASE ((uintptr_t)regs)
#include "kernel/arch/riscv64/boards/ox64/ox64_console.h"
#include "firmware/devicetree.h"
void reset(void) { memset((void *)regs, 0, sizeof(regs)); }
void set(unsigned offset, uint32_t value) { regs[offset/4] = value; }
uint32_t get(unsigned offset) { return regs[offset/4]; }
void init(void) { arch_uart_init(); }
void output(int c) { arch_uart_putc(c); }
int input(void) { return arch_uart_poll_getc(); }
void flush(void) { arch_uart_flush(); }
int memory(const void *b, size_t n, uint64_t *result) {
    fw_dt_t tree;
    int rc = fw_dt_init(&tree, b, n);
    fw_dt_range_t ranges[16]; size_t count = 0;
    if (!rc) rc = fw_dt_memory_window(&tree, 0x53800000,
        0x50000000, 0x53f00000, 0x50200000, 0x50800000, ranges, 16, &count, 0);
    if (!rc) { result[0] = ranges[0].base; result[1] = ranges[0].base + ranges[0].size; }
    return rc;
}
''')
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                        "-I", str(ROOT), "-I", str(ROOT / "kernel/include"), str(c),
                        str(ROOT / "kernel/core/fdt_view.c"), str(ROOT / "kernel/firmware/devicetree/node.c"),
                        str(ROOT / "kernel/firmware/devicetree/memory.c"), str(ROOT / "kernel/firmware/devicetree/framebuffer.c"), str(ROOT / "kernel/boot/framebuffer.c"), "-o", str(p / "wrapper.so")], check=True)
        cls.lib = ctypes.CDLL(str(p / "wrapper.so"))
        cls.lib.memory.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.get.restype = ctypes.c_uint32

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_bl808_uart_preserves_firmware_divisor_and_uses_32bit_fifo(self):
        l = self.lib
        l.reset()
        l.set(8, 0x00130013)  # 40MHz / 2Mbaud - 1 in both halves.
        l.set(0x84, 0x1f000020)
        l.init()
        self.assertEqual(l.get(8), 0x00130013)
        self.assertEqual(l.get(0x2c), 1 << 3)
        self.assertEqual(l.get(0x24), 0xf7)
        self.assertEqual(l.get(0x84), 32)
        l.output(ord("A"))
        self.assertEqual(l.get(0x88), ord("A"))
        self.assertEqual(l.input(), -1)
        l.set(0x84, 0x120)
        l.set(0x8c, 0x142)
        self.assertEqual(l.input(), ord("B"))
        l.set(0x84, 32)
        l.flush()

    def test_vendor_dtb_reservation_insertion_and_production_memory_parser(self):
        original = tree()
        blob = ox64.reserve_m0(original)
        h = struct.unpack_from(">10I", blob)
        self.assertEqual(h[1], len(original) + 16)
        self.assertEqual(struct.unpack_from(">QQ", blob, h[4]), (0x53f00000, 0x100000))
        result = (ctypes.c_uint64 * 2)()
        data = ctypes.create_string_buffer(blob)
        self.assertEqual(self.lib.memory(data, len(blob), result), 0)
        # DTB must remain live; allocator excludes it as well as the M0 tail.
        self.assertEqual(list(result), [0x50000000, 0x53800000])
        self.assertEqual(self.lib.memory(None, 0, result), -22)

    def test_wrong_board_and_bad_reserve_map_are_rejected(self):
        for data in [b"bad", tree().replace(b"pine64,ox64", b"pine64,other"),
                     tree()[:48]]:
            with self.assertRaises(ox64.Ox64Error):
                ox64.reserve_m0(data)

    def test_legacy_script_format_header_and_payload_crc(self):
        text = ox64.boot_script(b"kernel", tree())
        data = ox64.script_image(text)
        h = struct.unpack_from(">7I4B32s", data)
        self.assertEqual((h[0], h[3], h[7:11]), (0x27051956, len(data)-64, (5,26,6,0)))
        cleared = data[:4] + bytes(4) + data[8:64]
        self.assertEqual(h[1], zlib.crc32(cleared))
        self.assertEqual(h[6], zlib.crc32(data[64:]))
        self.assertEqual(struct.unpack_from(">II", data, 64), (len(text), 0))
        self.assertEqual(data[72:], text.encode())
        self.assertIn("itest.l *0x50010000", text)
        self.assertNotIn("crc32 -v", text)
        self.assertIn("fatload mmc 0:1", text)
        self.assertIn("setenv fdt_high 0xffffffffffffffff", text)

    def test_mbr_bootable_partition_and_no_fake_spi_image(self):
        b = ox64.mbr(128 << 20)
        status, _, kind, _, start, count = struct.unpack_from("<B3sB3sII", b, 446)
        self.assertEqual((status, kind, start), (0x80, 0x0c, 16384))
        self.assertEqual((start+count)*512, 128 << 20)
        self.assertEqual(b[510:], b"\x55\xaa")
        for bad in [0, 64 << 20, (128 << 20)+1]:
            with self.assertRaises(ox64.Ox64Error):
                ox64.mbr(bad)
        with self.assertRaises(ox64.Ox64Error):
            ox64.regular(Path("/dev/null"))

    def test_recovery_is_print_only_until_explicit_flash_size_and_execute(self):
        a = SimpleNamespace(firmware=Path("/unused"), tool="bflb-iot-tool", port="/dev/NEVER",
                            baud=115200, flash_mib=None, execute=False)
        with patch.object(ox64, "validate_assets"), patch.object(ox64.subprocess, "run") as run, redirect_stdout(io.StringIO()):
            ox64.recovery(a)
            run.assert_not_called()
            a.execute=True
            with self.assertRaises(ox64.Ox64Error):
                ox64.recovery(a)
            run.assert_not_called()

    def test_vendor_zero_exit_without_write_completion_is_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            # These are host-only fake CLI programs: no serial/device opened.
            failed = [sys.executable, "-c", "print('ErrorCode: 0050')"]
            success = [sys.executable, "-c", "print('[All Successful]')"]
            with redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ox64.Ox64Error, "not confirmed"):
                    ox64.run_flasher(failed, folder / "failed.log")
                ox64.run_flasher(success, folder / "ok.log")
            self.assertIn("ErrorCode", (folder / "failed.log").read_text())
            self.assertIn("All Successful", (folder / "ok.log").read_text())

    def test_cached_release_digest_rejected_before_extract(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            (p/ox64.ARCHIVE).write_bytes(b"corrupt")
            with self.assertRaisesRegex(ox64.Ox64Error, "digest mismatch"):
                ox64.prepare(SimpleNamespace(cache=p, out=p/"out"))

    def test_dtb_and_bss_collision_rejected_before_media_creation(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);fw=p/"firmware";fw.mkdir();bundle=p/"handoff";bundle.mkdir()
            (fw/ox64.DTB_NAME).write_bytes(tree())
            kernel=bytearray(64)
            struct.pack_into("<QQ", kernel, 8, 0x200000, ox64.FDT-ox64.LOAD)
            kernel[48:60]=b"RISCV\0\0\0RSC\x05"
            (bundle/"kernel.bin").write_bytes(kernel)
            (bundle/"manifest.json").write_text(json.dumps({"board":"ox64", "load_address":hex(ox64.LOAD),
                "files":{"kernel.bin":ox64.artifact(bundle/"kernel.bin")}}))
            with patch.object(ox64,"validate_assets", return_value={}):
                with self.assertRaisesRegex(ox64.Ox64Error,"overlaps"):
                    ox64.sdcard(SimpleNamespace(firmware=fw,bundle=bundle,output=p/"sd.img",size_mib=128))
            self.assertFalse((p/"sd.img").exists())

if __name__ == "__main__":
    unittest.main()
