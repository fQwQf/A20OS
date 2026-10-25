"""Actual firmware-output contracts, fail-closed RAM selection, and TFTP I/O."""
import json
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ls2k0300_handoff as handoff
from mk_ls2k0300_bundle import inspect_elf

BDINFO = '''DRAM bank   = 0x0000000000000000
-> start    = 0x9000000000000000
-> size     = 0x0000000010000000
relocaddr   = 0x900000000ec00000
fdt_blob    = 0x900000000d100000
new_fdt     = 0x0000000000000000
fdt_size    = 0x0000000000010000
lmb_dump_all:
 memory.cnt  = 0x1
 memory[0] [0x9000000000000000-0x900000000fffffff], 0x10000000 bytes flags: 0
 reserved.cnt  = 0x1
 reserved[0] [0x900000000d000000-0x900000000fffffff], 0x03000000 bytes flags: 0
'''


def bundle(root):
    raw = bytearray(184)
    raw[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<HH', raw, 16, 2, 258)
    struct.pack_into('<QQ', raw, 24, 0x9000000002000000, 64)
    struct.pack_into('<HH', raw, 54, 56, 1)
    struct.pack_into('<IIQQQQQQ', raw, 64, 1, 5, 120,
                     0x9000000002000000, 0x9000000002000000, 64, 4096, 8)
    raw[120:] = b'\x02' * 64
    (root / 'kernel.elf').write_bytes(raw)
    (root / 'image.json').write_text(json.dumps(inspect_elf(root / 'kernel.elf')))
    return bytes(raw)


class Clock:
    value = 0
    def __call__(self):
        return self.value


class Firmware:
    """UART fake supports the simple (non-Hush) vendor command parser."""
    def __init__(self, payload, *, bdinfo=BDINFO, failure=None, pmon=False):
        self.clock = Clock()
        self.payload = payload
        self.bdinfo = bdinfo
        self.failure = failure
        self.pmon = pmon
        self.writes = []
        self.queue = []
        self.filesize = None
        self.network = {}

    def read(self, timeout):
        self.clock.value += timeout
        return self.queue.pop(0) if self.queue else b''

    def write(self, data, timeout=10):
        command = data.decode().strip()
        self.writes.append(command)
        if self.pmon:
            response = 'PMON 2024\r\n' if command == 'c' else 'Boot and Load commands\r\n'
            self.queue.append((response + 'PMON> ').encode())
            return
        result = ''
        if command == 'c':
            result = "Unknown command 'c'\n"
        elif command == 'version':
            result = 'U-Boot 2022.04-v2.0.0 LA264\n'
        elif command == 'bdinfo':
            result = self.bdinfo
        elif command.startswith('help '):
            result = command[5:] + ' [-p|-s] supported command\n'
        elif command == 'printenv filesize':
            result = f'filesize={self.filesize:x}\n' if self.filesize is not None else '## Error: filesize not defined\n'
        elif command.startswith(('setenv serverip ', 'setenv ipaddr ')):
            _, key, value = command.split(); self.network[key] = value
        elif command == 'printenv serverip ipaddr':
            result = '\n'.join(f'{key}={value}' for key, value in self.network.items()) + '\n'
        elif command.startswith('tftpboot '):
            self.filesize = len(self.payload)
            result = f'Bytes transferred = {len(self.payload)} ({len(self.payload):x} hex)\n'
        elif command.startswith('setenv filesize'):
            self.filesize = None
        elif command.startswith('size '):
            self.filesize = len(self.payload)
        elif command.startswith('load '):
            if self.failure == 'load':
                result = '** File not found /a20/kernel.elf **\n'
            else:
                self.filesize = len(self.payload)
                result = f'{len(self.payload)} bytes read in 10 ms\n'
        elif command.startswith('crc32 '):
            crc = 0 if self.failure == 'crc' else zlib.crc32(self.payload)
            result = f'CRC32 for 9000000008000000 ... ==> {crc:08x}\n'
        elif command.startswith('bootelf -p '):
            self.queue.extend([b'## Starting application at 0x9000000002000000 ...\r\n',
                               b'[LS2K0300] stage=irq\r\nSystem ready (bringup, no userspace)\r\npart ok\r\n'])
            return
        # Split the prompt across reads to exercise the streaming recognizer.
        response = command + '\r\n' + result + '\r\n=> '
        self.queue.extend([response[:-2].encode(), response[-2:].encode()])


class TestMemory(unittest.TestCase):
    def test_selects_safe_cached_region_above_allocator(self):
        plan = handoff.plan_memory(BDINFO, 0x512345)
        self.assertEqual(plan.staging, 0x9000000008000000)

    def test_no_lmb_no_load(self):
        with self.assertRaisesRegex(ValueError, 'missing LMB'):
            handoff.plan_memory(BDINFO.split('lmb_dump_all')[0], 1024)

    def test_incomplete_reserved_table(self):
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            handoff.plan_memory(BDINFO.replace('reserved.cnt  = 0x1', 'reserved.cnt  = 0x2'), 1024)

    def test_live_reservation_in_allocator_is_rejected(self):
        text = BDINFO.replace('900000000d000000', '9000000007000000').replace('03000000 bytes', '09000000 bytes')
        with self.assertRaisesRegex(ValueError, 'overlap'):
            handoff.plan_memory(text, 1024)

    def test_small_ddr_does_not_get_default_capacity(self):
        text = BDINFO.replace('-> size     = 0x0000000010000000', '-> size     = 0x0000000004000000')
        with self.assertRaisesRegex(ValueError, 'DDR covers'):
            handoff.plan_memory(text, 1024)


class TestProtocol(unittest.TestCase):
    def execute(self, **options):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = bundle(root)
            firmware = Firmware(payload, **options)
            result = handoff.run_handoff(firmware, bundle=root, now=firmware.clock,
                                        command_timeout=1, boot_timeout=1)
            return result, firmware

    def test_complete_storage_boot(self):
        result, uart = self.execute()
        self.assertTrue(result.ok, result.missing)
        self.assertEqual(result.stage, 'ls2k0300:boot')
        self.assertTrue(any(c.startswith('load mmc 0:1 9000000008000000 ') for c in uart.writes))
        self.assertTrue(uart.writes[-1].startswith('bootelf -p '))
        self.assertNotIn('&&', '\n'.join(uart.writes))
        self.assertIn('[HOST] LS2K0300 proven staging=', result.transcript.text)

    def test_load_failure_never_sends_boot(self):
        result, uart = self.execute(failure='load')
        self.assertFalse(result.ok)
        self.assertFalse(any(c.startswith('bootelf ') for c in uart.writes))
        self.assertIn('File not found', result.transcript.text)

    def test_crc_failure_never_sends_boot(self):
        result, uart = self.execute(failure='crc')
        self.assertFalse(result.ok)
        self.assertEqual(result.stage, 'ls2k0300:crc')
        self.assertFalse(any(c.startswith('bootelf ') for c in uart.writes))

    def test_unproven_ddr_never_loads(self):
        result, uart = self.execute(bdinfo=BDINFO.split('lmb_dump_all')[0])
        self.assertEqual(result.stage, 'ls2k0300:memory')
        self.assertFalse(any(c.startswith(('load ', 'tftpboot ')) for c in uart.writes))

    def test_pmon_only_captures_help(self):
        result, uart = self.execute(pmon=True)
        self.assertEqual(result.stage, 'ls2k0300:pmon')
        self.assertEqual(uart.writes, ['c', 'h'])

    def test_complete_tftp_boot_with_verified_network_readback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); payload = bundle(root); uart = Firmware(payload)
            result = handoff.run_handoff(uart, bundle=root, now=uart.clock, transfer='tftp',
                                        server_ip='192.168.1.2', board_ip='192.168.1.178')
            self.assertTrue(result.ok, result.missing)
            self.assertTrue(any(c.startswith('tftpboot 9000000008000000 kernel.elf') for c in uart.writes))

    def test_tftp_without_network_configuration_never_touches_uart(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); payload = bundle(root); uart = Firmware(payload)
            result = handoff.run_handoff(uart, bundle=root, now=uart.clock, transfer='tftp')
            self.assertFalse(result.ok)
            self.assertEqual(uart.writes, [])

    def test_keyboard_interrupt_at_prompt_preserves_received_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); payload = bundle(root); uart = Firmware(payload)
            with patch.object(uart, 'read', side_effect=[b'bootloader partial output\r\n', KeyboardInterrupt]):
                result = handoff.run_handoff(uart, bundle=root, now=uart.clock)
            self.assertFalse(result.ok)
            self.assertEqual(result.stage, 'ls2k0300:prompt')
            self.assertIn('interrupted', result.missing[0])
            self.assertIn('bootloader partial output', result.transcript.text)

    def test_keyboard_interrupt_at_boot_preserves_inventory_and_boot_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); payload = bundle(root); uart = Firmware(payload)
            original_read = uart.read
            reads_after_boot = 0
            def interrupt_boot(timeout):
                nonlocal reads_after_boot
                if uart.writes and uart.writes[-1].startswith('bootelf '):
                    reads_after_boot += 1
                    if reads_after_boot == 2:
                        raise KeyboardInterrupt
                return original_read(timeout)
            with patch.object(uart, 'read', side_effect=interrupt_boot):
                result = handoff.run_handoff(uart, bundle=root, now=uart.clock)
            self.assertFalse(result.ok)
            self.assertEqual(result.stage, 'ls2k0300:boot')
            self.assertIn('lmb_dump_all', result.transcript.text)
            self.assertIn('Starting application', result.transcript.text)
            self.assertIn('interrupted', result.missing[0])

    def test_modified_elf_is_rejected_before_uart(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); payload = bundle(root)
            (root / 'image.json').write_text('{"sha256":"wrong"}')
            uart = Firmware(payload)
            result = handoff.run_handoff(uart, bundle=root, now=uart.clock)
            self.assertEqual(result.stage, 'ls2k0300:bundle')
            self.assertEqual(uart.writes, [])


class TestStaging(unittest.TestCase):
    def test_copy_and_idempotent_repeat(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'bundle'; source.mkdir()
            payload = bundle(source); mount = root / 'media'; mount.mkdir()
            with patch('m0sense_deploy_uf2.mounted_fat_volumes', return_value=[mount]):
                first = handoff.stage_bundle(source, mount, False)
                self.assertTrue(first['copied'])
                self.assertEqual((mount / 'a20/kernel.elf').read_bytes(), payload)
                second = handoff.stage_bundle(source, mount, False)
                self.assertTrue(second['already_present'])
                self.assertFalse(second['copied'])

    def test_different_existing_file_is_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'bundle'; source.mkdir(); bundle(source)
            mount = root / 'media'; mount.mkdir(); (mount / 'a20').mkdir()
            target = mount / 'a20/kernel.elf'; target.write_bytes(b'previous user kernel')
            with patch('m0sense_deploy_uf2.mounted_fat_volumes', return_value=[mount]):
                with self.assertRaisesRegex(ValueError, 'refusing to overwrite'):
                    handoff.stage_bundle(source, mount, False)
            self.assertEqual(target.read_bytes(), b'previous user kernel')

    def test_dry_run_creates_no_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'bundle'; source.mkdir(); bundle(source)
            mount = root / 'media'; mount.mkdir()
            with patch('m0sense_deploy_uf2.mounted_fat_volumes', return_value=[mount]):
                result = handoff.stage_bundle(source, mount, True)
            self.assertFalse(result['staged'])
            self.assertFalse((mount / 'a20').exists())

    def test_regular_host_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'bundle'; source.mkdir(); bundle(source)
            mount = root / 'media'; mount.mkdir()
            with patch('m0sense_deploy_uf2.mounted_fat_volumes', return_value=[]):
                with self.assertRaisesRegex(ValueError, 'actually mounted'):
                    handoff.stage_bundle(source, mount, False)
            self.assertFalse((mount / 'a20').exists())

    def test_symlink_mount_directory_and_destination_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'bundle'; source.mkdir(); bundle(source)
            mount = root / 'media'; mount.mkdir(); outside = root / 'outside'; outside.mkdir()
            link = root / 'link'; link.symlink_to(mount, target_is_directory=True)
            with patch('m0sense_deploy_uf2.mounted_fat_volumes', return_value=[mount]):
                with self.assertRaisesRegex(ValueError, 'symlink'):
                    handoff.stage_bundle(source, link, False)
                (mount / 'a20').symlink_to(outside, target_is_directory=True)
                with self.assertRaisesRegex(ValueError, 'real directory'):
                    handoff.stage_bundle(source, mount, False)
                (mount / 'a20').unlink(); (mount / 'a20').mkdir()
                victim = outside / 'victim'; victim.write_bytes(b'keep')
                (mount / 'a20/kernel.elf').symlink_to(victim)
                with self.assertRaises((ValueError, OSError)):
                    handoff.stage_bundle(source, mount, False)
            self.assertEqual(victim.read_bytes(), b'keep')

    def test_invalid_bundle_never_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'bundle'; source.mkdir(); bundle(source)
            (source / 'image.json').write_text('{"sha256":"wrong"}')
            mount = root / 'media'; mount.mkdir()
            with patch('m0sense_deploy_uf2.mounted_fat_volumes', return_value=[mount]):
                with self.assertRaisesRegex(ValueError, 'hash differs'):
                    handoff.stage_bundle(source, mount, False)
            self.assertFalse((mount / 'a20').exists())


class TestManualFirmwareServer(unittest.TestCase):
    def test_wrong_hash_never_starts_server(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'u-boot-spl-gz.bin'; path.write_bytes(b'example')
            with patch.object(handoff, '_serve_bytes') as server:
                with self.assertRaisesRegex(ValueError, 'SHA-256'):
                    handoff.serve_firmware(path, '0' * 64, '127.0.0.1', 1069)
                server.assert_not_called()

    def test_other_filenames_are_not_served_as_firmware(self):
        with self.assertRaisesRegex(ValueError, 'only u-boot-spl-gz.bin'):
            handoff.serve_firmware(Path('/unused/other.bin'), '0' * 64)


class TestTFTP(unittest.TestCase):
    def test_real_udp_transfer_and_path_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); payload = bundle(root)
            ready = threading.Event(); endpoint = []
            def started(address):
                endpoint.append(address); ready.set()
            thread = threading.Thread(target=handoff.serve_tftp,
                                      args=(root, '127.0.0.1', 0),
                                      kwargs={'once': True, 'on_ready': started}, daemon=True)
            thread.start(); self.assertTrue(ready.wait(2))
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.settimeout(2)
                client.sendto(b'\0\1/etc/passwd\0octet\0', endpoint[0])
                response, _ = client.recvfrom(2048)
                self.assertEqual(response[:2], b'\0\5')
                client.sendto(b'\0\1kernel.elf\0octet\0blksize\01468\0', endpoint[0])
                response, transfer = client.recvfrom(2048)
                self.assertEqual(response[:4], b'\0\3\0\1')
                self.assertEqual(response[4:], payload)
                client.sendto(b'\0\4\0\1', transfer)
            thread.join(2); self.assertFalse(thread.is_alive())


if __name__ == '__main__':
    unittest.main()
