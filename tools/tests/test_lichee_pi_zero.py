"""ARM32 V3s boot image contracts; no physical board emulation."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import hashlib
import json
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/cli'))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "boards"))
import lichee_pi_zero as v3s


def elf(size=0x800000, address=v3s.LOAD, entry=v3s.LOAD):
    data = bytearray(128)
    data[:6] = b'\x7fELF\x01\x01'
    struct.pack_into('<H', data, 18, 40)
    struct.pack_into('<II', data, 24, entry, 52)
    struct.pack_into('<HH', data, 42, 32, 1)
    struct.pack_into('<6I', data, 52, 1, 128, 0xc0080000, address, 0, size)
    return bytes(data)


def uimage(kernel=b'kernel'):
    header = struct.pack('>7I4B32s', 0x27051956, 0, 0, len(kernel), v3s.LOAD, v3s.LOAD,
                         zlib.crc32(kernel), 5, 2, 2, 0, b'A20OS V3s')
    header = header[:4] + struct.pack('>I', zlib.crc32(header)) + header[8:]
    return header + kernel


class LicheePiZeroImages(unittest.TestCase):
    def test_elf32_entry_and_complete_bss_footprint(self):
        self.assertEqual(v3s.elf_layout(elf()), (v3s.LOAD, 0x800000))

    def test_wrong_entry_load_and_fdt_overlap_fail_closed(self):
        for data in (elf(entry=0xc0080000), elf(address=0x41000000), elf(size=v3s.FDT-v3s.LOAD), b'bad'):
            with self.assertRaises(v3s.common.PrepareError):
                v3s.elf_layout(data)

    def test_legacy_image_checks_header_data_crc_and_load(self):
        good = uimage()
        v3s.validate_uimage(good, b'kernel')
        for index in (4, 8, 16, 20, 24, 28, 29, 30, 31, 64):
            changed = bytearray(good)
            changed[index] ^= 1
            with self.assertRaises(v3s.common.PrepareError):
                v3s.validate_uimage(bytes(changed), b'kernel')

    def test_kernel_bytes_must_match_elf_not_another_build(self):
        data = bytearray(elf()) + b'kernel'
        struct.pack_into('<I', data, 52 + 16, 6)
        v3s.validate_binary(bytes(data), b'kernel')
        for bad in (b'KERnel', b'kernel-extra', b''):
            with self.assertRaises(v3s.common.PrepareError):
                v3s.validate_binary(bytes(data), bad)

    def test_boot_script_uses_bootm_and_checked_payload(self):
        script = v3s.boot_script(uimage(), b'dtb')
        self.assertIn('bootm ${loadaddr} - ${fdt_addr_r}', script)
        self.assertIn('loadaddr 0x42000000', script)
        self.assertIn('crc32 -v', script)
        self.assertIn('itest ${filesize}', script)
        self.assertNotIn('booti ', script)

    def test_real_sd_recipe_matches_payload_contract(self):
        import tomllib
        recipe = tomllib.loads((ROOT / 'instances/lichee-pi-zero.toml').read_text())
        command = '\n'.join(recipe['handoff']['commands'])
        self.assertIn('setenv loadaddr 0x42000000', command)
        self.assertIn('kernel.uimg', command)
        self.assertIn(v3s.DTB_NAME, command)
        self.assertIn('bootm ${loadaddr} - ${fdt_addr_r}', command)
        from a20_handoff import load_plan
        with tempfile.TemporaryDirectory() as work:
            bundle = Path(work)
            files = {'kernel.bin': b'kernel', 'kernel.elf': b'fixture',
                     'kernel.uimg': uimage(), v3s.DTB_NAME: b'dtb',
                     'uboot.cmd': (command + '\n').encode()}
            records = {}
            for name, data in files.items():
                (bundle / name).write_bytes(data)
                records[name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            (bundle / 'manifest.json').write_text(json.dumps({
                'board': recipe['board'], 'load_address': hex(v3s.LOAD), 'image_size': 0x1000000,
                'boot_protocol': 'bootm', 'payload_name': 'kernel.uimg',
                'payload_load_address': hex(v3s.PAYLOAD), 'files': records}))
            commands, meta = load_plan(bundle, recipe['board'])
            self.assertEqual(commands, recipe['handoff']['commands'])
            self.assertEqual(meta['payload_name'], 'kernel.uimg')


if __name__ == '__main__':
    unittest.main()
