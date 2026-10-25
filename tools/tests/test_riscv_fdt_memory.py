"""Synthetic DTBs exercise production RISC-V RAM/firmware reservation logic."""
from __future__ import annotations

import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
U32 = lambda x: struct.pack(">I", x)
U64 = lambda x: struct.pack(">Q", x)


def dtb(*, reservations=(), child_reservations=(), reserved_first=False,
        ram_base=0x80000000, ram_size=0x10000000, cells=2,
        dynamic=False, ranges=b"", truncate_reg=False):
    strings = bytearray()
    names = {}
    def name(s):
        if s not in names:
            names[s] = len(strings)
            strings.extend(s.encode() + b"\0")
        return names[s]
    def pad(b):
        return b + b"\0" * (-len(b) % 4)
    def node(s, *children):
        return U32(1) + pad(s.encode() + b"\0") + b"".join(children) + U32(2)
    def prop(s, data):
        return U32(3) + U32(len(data)) + U32(name(s)) + pad(data)
    encode = U64 if cells == 2 else U32
    memreg = encode(ram_base) + encode(ram_size)
    if truncate_reg:
        memreg = memreg[:-1]
    memory = node("memory@80000000", prop("device_type", b"memory\0"), prop("reg", memreg))
    children = [node(f"rsv@{base:x}", prop("reg", encode(base) + encode(size)))
                for base, size in child_reservations]
    if dynamic:
        children.append(node("dynamic", prop("size", encode(0x1000))))
    reserved = node("reserved-memory", prop("#address-cells", U32(cells)),
                    prop("#size-cells", U32(cells)), prop("ranges", ranges), *children)
    order = (reserved, memory) if reserved_first else (memory, reserved)
    structure = node("", prop("#address-cells", U32(cells)), prop("#size-cells", U32(cells)), *order) + U32(9)
    reserves = b"".join(U64(a) + U64(n) for a, n in reservations) + bytes(16)
    st = 40 + len(reserves)
    str_offset = st + len(structure)
    total = str_offset + len(strings)
    header = struct.pack(">10I", 0xd00dfeed, total, st, str_offset, 40, 17, 16, 0,
                         len(strings), len(structure))
    return header + reserves + structure + strings


class RiscvFdtMemory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="a20-riscv-memory-")
        wrapper = Path(cls.temp.name) / "wrapper.c"
        wrapper.write_text('''
#include "firmware/devicetree.h"
int window(const void *blob, size_t bytes, uint64_t physical,
           uint64_t base, uint64_t end, uint64_t *result) {
    fw_dt_t tree; fw_dt_range_t ranges[16]; size_t count = 0;
    int rc = fw_dt_init(&tree, blob, bytes);
    if (!rc) rc = fw_dt_memory_window(&tree, physical, base, end,
        base + 0x200000, base + 0x2000000, ranges, 16, &count, 0);
    result[0] = count;
    if (!rc) for (size_t i = 0; i < count; i++) {
        result[1 + i*2] = ranges[i].base;
        result[2 + i*2] = ranges[i].base + ranges[i].size;
    }
    return rc;
}
''')
        so = Path(cls.temp.name) / "memory.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-std=gnu99", "-O2", "-fno-builtin",
                        "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "kernel/include"),
                        str(wrapper), str(ROOT / "kernel/core/fdt_view.c"),
                        str(ROOT / "kernel/firmware/devicetree/node.c"),
                        str(ROOT / "kernel/firmware/devicetree/memory.c"), str(ROOT / "kernel/firmware/devicetree/framebuffer.c"), str(ROOT / "kernel/boot/framebuffer.c"), "-o", str(so)], check=True)
        cls.lib = ctypes.CDLL(str(so))
        cls.lib.window.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64,
                                  ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.window.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def window(self, blob, physical=0x8f000000, base=0x80000000, end=0x90000000):
        result = (ctypes.c_uint64 * 33)()
        data = ctypes.create_string_buffer(blob) if blob is not None else None
        rc = self.lib.window(data, len(blob) if blob else 0, physical, base, end, result)
        ranges = [(result[1 + i*2], result[2 + i*2]) for i in range(result[0])]
        return rc, ranges

    def test_missing_dtb_is_not_hidden_by_common_parser(self):
        # The architecture owns explicit no-firmware fallback; this helper does not.
        self.assertEqual(self.window(None), (-22, []))

    def test_dtb_itself_survives_allocator_on_both_sides(self):
        blob = dtb()
        start = (0x8f000000 + len(blob) + 4095) & ~4095
        self.assertEqual(self.window(blob), (0, [(0x80000000, 0x8f000000),
                                                (start, 0x90000000)]))

    def test_memreserve_firmware_and_top(self):
        blob = dtb(reservations=[(0x80000000, 0x200000), (0x8c000000, 0x4000000)])
        self.assertEqual(self.window(blob), (0, [(0x80200000, 0x8c000000)]))

    def test_reserved_nodes_before_memory_and_unaligned(self):
        blob = dtb(child_reservations=[(0x84000123, 0x2345)], reserved_first=True)
        rc, ranges = self.window(blob, physical=0x90000000)
        self.assertEqual((rc, ranges), (0, [(0x80000000, 0x84000000),
                                          (0x84003000, 0x90000000)]))

    def test_reservation_boundaries_and_order(self):
        blob = dtb(reservations=[(0x90000000, 4096), (0x7ffff000, 4096),
                                  (0x88000000, 4096), (0x85000000, 4096)])
        self.assertEqual(self.window(blob, physical=0x91000000), (0, [
            (0x80000000, 0x85000000), (0x85001000, 0x88000000),
            (0x88001000, 0x90000000)]))

    def test_kernel_overlap_rejected_in_each_reservation_source(self):
        for kw in ({"reservations": [(0x801ff000, 0x2000)]},
                   {"child_reservations": [(0x81fff000, 0x2000)]}):
            self.assertEqual(self.window(dtb(**kw)), (-14, []))
        self.assertEqual(self.window(dtb(), physical=0x81fffff0), (-14, []))

    def test_32bit_cells_and_memory_clamping(self):
        self.assertEqual(self.window(dtb(cells=1, ram_size=0x8000000)),
                         (0, [(0x80000000, 0x88000000)]))

    def test_vf2_ram_window_starts_below_qemu(self):
        blob = dtb(ram_base=0x40000000, ram_size=0x200000000,
                   reservations=[(0x40000000, 0x200000)])
        start = (0x46000000 + len(blob) + 4095) & ~4095
        self.assertEqual(self.window(blob, physical=0x46000000, base=0x40000000,
                                     end=0x240000000), (0, [
            (0x40200000, 0x46000000), (start, 0x240000000)]))

    def test_unsafe_or_malformed_descriptions_rejected(self):
        fixtures = [dtb(dynamic=True), dtb(ranges=bytes(16), child_reservations=[(0x84000000,4096)]),
                    dtb(truncate_reg=True), dtb(ram_size=0x1000000),
                    dtb(child_reservations=[(0x8ffff000, 0)]),
                    dtb(reservations=[(0xfffffffffffff000, 0x2000)])]
        for blob in fixtures:
            self.assertLess(self.window(blob)[0], 0)
        valid = dtb()
        for size in range(len(valid)):
            self.assertLess(self.window(valid[:size])[0], 0, size)


if __name__ == "__main__":
    unittest.main()
