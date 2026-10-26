"""UEFI world oracle and QMP tests; never start QEMU or build shared targets."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
try:
    spec = importlib.util.spec_from_file_location('check_uefi_world', ROOT / 'tools/check_uefi_world.py')
    smoke = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(smoke)
finally:
    sys.path.pop(0)

MARKER = 'a20-uefi-world-1234567890abcdef'
COMPLETE = '\r\n'.join((*smoke.REQUIRED_BOOT, MARKER,
    'System is going down for power-off NOW.', '[SD] POWER_OFF entry[SD] audit errors=0'))


def ppm(width, height, pixels, header=None):
    return (header or f'P6\n{width} {height}\n255\n'.encode()) + pixels


class FakeStream:
    def __init__(self, replies):
        self.replies = list(replies)
        self.requests = []
        self.closed = False

    def readline(self, limit):
        if not self.replies:
            return b''
        answer = self.replies.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer if isinstance(answer, bytes) else (json.dumps(answer)+'\n').encode()

    def write(self, data):
        self.requests.append(json.loads(data))
        return len(data)

    def close(self):
        self.closed = True


class FakeSocket:
    def __init__(self, replies):
        self.stream = FakeStream(replies)
        self.closed = False

    def connect(self, path):
        self.path = path

    def makefile(self, mode, buffering):
        assert mode == 'rwb' and buffering == 0
        return self.stream

    def sendall(self, data):
        self.stream.write(data)

    def settimeout(self, timeout):
        assert 0 < timeout <= 10

    def close(self):
        self.closed = True


class UefiWorldOracle(unittest.TestCase):
    def test_success_requires_actual_execution_and_shutdown(self):
        smoke.validate_output(COMPLETE, 0, MARKER)
        smoke.validate_output(COMPLETE.replace('System is going down for power-off NOW.', ''), 0, MARKER)

    def test_typed_marker_and_serial_echo_are_insufficient(self):
        for replacement in ('echo '+MARKER+' >/dev/console', '# '+MARKER, MARKER+' trailing'):
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                smoke.validate_output(COMPLETE.replace(MARKER, replacement), 0, MARKER)

    def test_guest_exit_status_and_crash_diagnostics_fail(self):
        for status, text in ((1, COMPLETE), (-15, COMPLETE),
                             (0, COMPLETE+'\nKERNEL PANIC'),
                             (0, COMPLETE+'\nUBSAN: bad index'),
                             (0, COMPLETE+'\nSIGSEGV'),
                             (0, COMPLETE+'\nADE/ALE: pid=7')):
            with self.subTest(status=status, text=text), self.assertRaises(ValueError):
                smoke.validate_output(text, status, MARKER)

    def test_all_boot_proofs_are_required(self):
        for marker in smoke.REQUIRED_BOOT:
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                smoke.validate_output(COMPLETE.replace(marker, ''), 0, MARKER)

    def test_shutdown_audit_cannot_be_missing_or_hidden_by_a_later_zero(self):
        for text in (COMPLETE.replace('[SD] audit errors=0', ''),
                     COMPLETE.replace('errors=0', 'errors=1'),
                     COMPLETE+'\n[SD] audit errors=2',
                     COMPLETE.replace('[SD] POWER_OFF entry', '')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                smoke.validate_output(text, 0, MARKER)

    def test_snapshot_command_uses_gop_ahci_and_usb_without_virtio(self):
        for acceleration in ('kvm', 'tcg'):
            command = smoke.qemu_command('qemu-system-x86_64', Path('/source/world.img'),
                Path('/source/code.fd'), Path('/temporary/vars.fd'), Path('/temporary/qmp.sock'),
                Path('/output/serial.log'), acceleration)
            self.assertEqual(command[command.index('-m')+1], '2048M')
            self.assertEqual(command[command.index('-accel')+1], acceleration)
            self.assertIn('pdpe1gb=off,avx=off,avx2=off', command[command.index('-cpu')+1])
            self.assertIn('if=none,id=world,format=raw,snapshot=on,file=/source/world.img', command)
            self.assertIn('if=pflash,format=raw,file=/temporary/vars.fd', command)
            self.assertIn('ide-hd,drive=world,bus=ide.0', command)
            self.assertIn('usb-kbd,bus=xhci.0', command)
            self.assertIn('name=opt/ovmf/X-PciMmio64Mb,string=0', command)
            self.assertNotIn('virtio', ' '.join(command))
            self.assertNotIn('stdio', command)
        self.assertEqual(smoke.drive_path(Path('/media/my,image.img')), '/media/my,,image.img')

    def test_usb_qcodes_cover_exact_command_and_shift_redirection(self):
        command = 'echo '+MARKER+' >/dev/console'
        for char in command:
            self.assertTrue(smoke.key_codes(char))
        self.assertEqual(smoke.key_codes('>'), ('shift', 'dot'))
        with self.assertRaises(ValueError):
            smoke.key_codes('\n')

    def test_pixel_oracle_requires_terminal_sized_change(self):
        with tempfile.TemporaryDirectory(prefix='a20-world-pixels-') as name:
            before, after = Path(name)/'before.ppm', Path(name)/'after.ppm'
            pixels = b'\x00\x00\x00' * (320*240)
            before.write_bytes(ppm(320, 240, pixels))
            after.write_bytes(ppm(320, 240, pixels))
            with self.assertRaises(ValueError):
                smoke.compare_screens(before, after)
            after.write_bytes(ppm(320, 240, b'\xff\xff\xff'+pixels[3:]))
            with self.assertRaises(ValueError):
                smoke.compare_screens(before, after)
            after.write_bytes(ppm(320, 240, b'\xff\xff\xff' * 1000 + pixels[3000:]))
            result = smoke.compare_screens(before, after)
            self.assertEqual(result['changed_pixels'], 1000)

    def test_ppm_pixel_whitespace_comments_and_truncation(self):
        with tempfile.TemporaryDirectory(prefix='a20-world-ppm-') as name:
            path = Path(name)/'screen.ppm'
            pixels = b'\n\x20\x09' * (320*240)
            path.write_bytes(ppm(320,240,pixels,b'P6\n# QMP screenshot\n320 240\n255\n'))
            self.assertEqual(smoke.read_ppm(path), (320,240,pixels))
            for data in (b'P3\n320 240\n255\n'+pixels,
                         b'P6\n320 240\n255\n'+pixels[:-1], b'P6\n# no newline'):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    smoke.read_ppm(path)

    def test_qmp_ignores_events_and_sends_only_qcodes(self):
        sock = FakeSocket([{'QMP': {}}, {'return': {}, 'id': 1},
                           {'event': 'RTC_CHANGE'}, {'return': {}, 'id': 2}])
        with patch.object(smoke.socket, 'socket', return_value=sock):
            control = smoke.QMP(Path('/unused/qmp'), time.monotonic()+30)
            control.send_key(('meta_l', 'ret'))
            control.close()
        request = sock.stream.requests[1]
        self.assertEqual(request['execute'], 'send-key')
        self.assertEqual(request['arguments']['keys'], [
            {'type':'qcode', 'data':'meta_l'}, {'type':'qcode', 'data':'ret'}])
        self.assertEqual(request['arguments']['hold-time'], 100)
        self.assertGreaterEqual(smoke.KEY_INTERVAL_SECONDS, .15)
        self.assertTrue(sock.closed and sock.stream.closed)

    def test_qmp_eof_error_timeout_and_wrong_ids_are_fatal(self):
        for reply in (b'', {'error': {'desc': 'bad command'}, 'id': 1},
                      {'return': {}, 'id': 99}, b'{bad json}\n', TimeoutError('timeout')):
            sock = FakeSocket([{'QMP': {}}, reply])
            with self.subTest(reply=reply), patch.object(smoke.socket, 'socket', return_value=sock):
                with self.assertRaises((ValueError, TimeoutError)):
                    smoke.QMP(Path('/unused/qmp'), time.monotonic()+30)
                self.assertTrue(sock.closed and sock.stream.closed)


if __name__ == '__main__':
    unittest.main()
