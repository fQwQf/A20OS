"""Real mtools ESP formatting stays inside the 1..255 MiB GPT partition."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]

@unittest.skipUnless(all(shutil.which(tool) for tool in ('mformat','mmd','mcopy','truncate','sha256sum')),
                     'mtools and image utilities are required')
class EfiEspImage(unittest.TestCase):
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
