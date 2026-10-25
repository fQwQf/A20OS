"""Firmware allocation must cover ELF BSS and reject malformed extents."""
import struct
import unittest
from tools.elf_load_span import load_span


def image(wide=True, big=False, memory=0x9000, physical=0x200000):
    endian = '>' if big else '<'
    ident = b'\x7fELF' + bytes([2 if wide else 1, 2 if big else 1, 1]) + bytes(9)
    hfmt = endian + ('HHIQQQIHHHHHH' if wide else 'HHIIIIIHHHHHH')
    pfmt = endian + ('IIQQQQQQ' if wide else 'IIIIIIII')
    hsize = 16 + struct.calcsize(hfmt)
    psize = struct.calcsize(pfmt)
    header = struct.pack(hfmt, 2, 62, 1, physical, hsize, 0, 0, hsize, psize, 1, 0, 0, 0)
    p = (1, 5, 0, physical, physical, hsize + psize, memory, 4096) if wide else (
        1, 0, physical, physical, hsize + psize, memory, 5, 4096)
    return ident + header + struct.pack(pfmt, *p)


class LoadSpanTests(unittest.TestCase):
    def test_full_memory_extent_for_both_classes_and_orders(self):
        for wide in (False, True):
            for big in (False, True):
                with self.subTest(wide=wide, big=big):
                    self.assertEqual(load_span(image(wide, big)), (0x200000, 0x9000))

    def test_malformed_or_truncated_input(self):
        data = image()
        for malformed in (b'', data[:15], data[:50], data[:-1], image(memory=1),
                          image(physical=(1 << 64) - 4096)):
            with self.subTest(size=len(malformed)), self.assertRaises(ValueError):
                load_span(malformed)


if __name__ == '__main__':
    unittest.main()
