#!/usr/bin/env python3
"""Guard the FADT offsets consumed by the x86 firmware parser.

The integration guest verifies the parser against QEMU's real FADT. This small
source-contract fixture makes the legacy field layout explicit and ensures the
parser does not regress to reading the preceding SMI command bytes as PM blocks.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/arch/x86_64/platform/firmware.c"


class FadtOffsetTests(unittest.TestCase):
    def test_parser_uses_acpi_fadt_offsets(self):
        source = SOURCE.read_text()
        fields = {
            name: int(offset)
            for name, offset in re.findall(
                r"out->(pm1a_evt|pm1a_cnt|pm2_cnt|pm1a_evt_len|pm1a_cnt_len|pm2_cnt_len|gpe0_blk|gpe1_blk)\s*=\s*\(uint(?:8|16)_t\)acpi_field\(fadt,\s*(\d+),",
                source,
            )
        }
        self.assertEqual(fields, {
            "pm1a_evt": 56,
            "pm1a_cnt": 64,
            "pm2_cnt": 72,
            "pm1a_evt_len": 88,
            "pm1a_cnt_len": 89,
            "pm2_cnt_len": 90,
            "gpe0_blk": 80,
            "gpe1_blk": 84,
        })

        # Put valid PM blocks beside unmistakable poison in the SMI/header area.
        table = bytearray(96)
        for offset, value in ((44, 0x1111), (52, 0x0302), (60, 0x2222),
                              (68, 0x3333), (72, 0x4444), (76, 0x55),
                              (77, 0x66), (78, 0x77)):
            table[offset:offset + 4] = value.to_bytes(4, "little")
        for offset, value, width in ((56, 0x500, 4), (64, 0x604, 4),
                                     (72, 0x600, 4), (80, 0xafe0, 4),
                                     (84, 0xafe8, 4), (88, 4, 1),
                                     (89, 2, 1), (90, 1, 1)):
            table[offset:offset + width] = value.to_bytes(width, "little")

        def read(offset, width):
            return int.from_bytes(table[offset:offset + width], "little")

        self.assertEqual(read(fields["pm1a_cnt"], 4), 0x604)
        self.assertEqual(read(fields["pm1a_evt"], 4), 0x500)
        self.assertEqual(read(fields["pm2_cnt"], 4), 0x600)
        self.assertEqual(read(fields["pm1a_cnt_len"], 1), 2)
        self.assertNotEqual(read(fields["pm1a_cnt"], 4), 0x302)


if __name__ == "__main__":
    unittest.main()
