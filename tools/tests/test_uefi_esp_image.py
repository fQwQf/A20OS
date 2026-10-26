"""Real mtools ESP formatting stays inside the 1..255 MiB GPT partition."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import hashlib
import zlib
ROOT=Path(__file__).resolve().parents[2]

@unittest.skipUnless(all(shutil.which(tool) for tool in ('mformat','mmd','mcopy','truncate','sha256sum')),
                     'mtools and image utilities are required')
class EfiEspImage(unittest.TestCase):
    @unittest.skipUnless(Path('/usr/sbin/parted').is_file() and
                         Path('/usr/sbin/mkfs.ext4').is_file(), 'parted and e2fsprogs required')
    def test_apk_world_is_a_separate_named_ext4_partition(self):
        with tempfile.TemporaryDirectory(prefix='a20-world-esp-') as name:
            work = Path(name)
            loader = work/'loader.efi'; loader.write_bytes(b'A20-loader')
            world = work/'world.img'
            with world.open('wb') as stream:
                stream.truncate(32 * 1048576)
            subprocess.run(['/usr/sbin/mkfs.ext4', '-q', '-F', str(world)], check=True)
            image = work/'media.img'
            subprocess.run(['sh', str(ROOT/'tools/mk_uefi_fat_image.sh'), str(loader),
                            str(image), '-', 'BOOTX64.EFI', str(world)], check=True,
                           capture_output=True, env={**os.environ,
                               'PATH':os.environ['PATH']+':/usr/sbin', 'TMPDIR':str(work)})
            with image.open('rb') as disk:
                disk.seek(512); header = disk.read(512)
                self.assertEqual(header[:8], b'EFI PART')
                size, crc = struct.unpack_from('<II', header, 12)
                check = bytearray(header[:size]); check[16:20] = bytes(4)
                self.assertEqual(zlib.crc32(check), crc)
                entry_lba = struct.unpack_from('<Q', header, 72)[0]
                count, entry_size, crc = struct.unpack_from('<III', header, 80)
                disk.seek(entry_lba * 512); entries = disk.read(count * entry_size)
                self.assertEqual(zlib.crc32(entries), crc)
                entry = entries[entry_size:entry_size+128]
                self.assertEqual(entry[:16], bytes.fromhex('af3dc60f838472478e793d69d8477de4'))
                self.assertEqual(entry[56:128].decode('utf-16-le').rstrip('\0'), 'A20OS distro')
                first, last = struct.unpack_from('<QQ', entry, 32)
                self.assertEqual(first * 512, 256 * 1048576)
                self.assertEqual((last-first+1)*512, world.stat().st_size)
                disk.seek(first * 512)
                self.assertEqual(hashlib.sha256(disk.read(world.stat().st_size)).digest(),
                                 hashlib.sha256(world.read_bytes()).digest())
            result = subprocess.run(['mcopy', '-i', str(image)+'@@1048576',
                                     '::/EFI/BOOT/BOOTX64.EFI', '-'], check=True, capture_output=True)
            self.assertEqual(result.stdout, loader.read_bytes())

    def test_invalid_world_does_not_replace_output(self):
        with tempfile.TemporaryDirectory(prefix='a20-bad-world-') as name:
            work = Path(name)
            loader = work/'loader'; loader.write_bytes(b'loader')
            world = work/'world'; world.write_bytes(b'not ext4')
            image = work/'image'; image.write_bytes(b'keep existing image')
            run = subprocess.run(['sh', str(ROOT/'tools/mk_uefi_fat_image.sh'), str(loader),
                                  str(image), '-', 'BOOTX64.EFI', str(world)], capture_output=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(image.read_bytes(), b'keep existing image')

    def test_embedded_root_esp_geometry_and_loader(self):
        with tempfile.TemporaryDirectory(prefix='a20-esp-') as name:
            work=Path(name)
            commands=work/'bin';commands.mkdir()
            # Partition creation is independently checked by the actual OVMF
            # integration. This fixture exercises the real FAT formatter,
            # file copy and embedded-root packaging without requiring parted.
            parted=commands/'parted'
            parted.write_text('#!/bin/sh\nexit 0\n');parted.chmod(0o755)
            loader=work/'loader.efi';loader.write_bytes(b'A20-ESP-loader-fixture')
            image=work/'media.img'
            subprocess.run(['sh',str(ROOT/'tools/mk_uefi_fat_image.sh'),str(loader),
                            str(image),'-','BOOTX64.EFI'],check=True,capture_output=True,
                           env={**os.environ,'PATH':str(commands)+os.pathsep+os.environ['PATH'],
                                'TMPDIR':str(work)})
            with image.open('rb') as disk:
                disk.seek(1048576);boot=disk.read(512)
            self.assertEqual(boot[510:512],b'\x55\xaa')
            self.assertEqual(struct.unpack_from('<H',boot,11)[0],512)
            sectors=struct.unpack_from('<I',boot,32)[0]
            self.assertEqual(sectors,254*1024*1024//512)
            self.assertLessEqual(1048576+sectors*512,image.stat().st_size-1048576)
            result=subprocess.run(['mcopy','-i',str(image)+'@@1048576',
                                   '::/EFI/BOOT/BOOTX64.EFI','-'],
                                  check=True,capture_output=True)
            self.assertEqual(result.stdout,loader.read_bytes())

if __name__=='__main__':unittest.main()
