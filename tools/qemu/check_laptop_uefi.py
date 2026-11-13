#!/usr/bin/env python3
"""Boot the laptop UEFI disk under OVMF and prove the three porting pieces.

The image is attached as an NVMe drive, which is the storage class this port
adds and also the class the firmware must chain-load from.  The test then:

  1. waits for the embedded RAM userland to exec mksh on the serial console;
  2. types ``echo ok`` through i8042 or through xHCI with i8042 disabled.
     --usb-keyboard attaches the keyboard to a root port, --usb-hub puts it
     behind a powered USB hub;
  3. requires the driver self-tests to pass: the three I2C-HID tests (report
     descriptor bit arithmetic, HID-over-I2C protocol against a mock adapter,
     multi-touch slots), the r8169 TX/RX loopback through a register-level
     device model, and the DesignWare I2C transfer engine against a mock
     I2C-HID slave;
  4. runs ``nvstress /dev/disk0 1200 8192``: 1200 two-page PRP reads, which
     is the completion-interrupt and PRP2 path a normal 4 KiB page-cache
     read never exercises (see docs/zh/platforms/x86_64-laptop.md);
  5. captures the QEMU scanout through the monitor and requires a non-blank
     frame, which is the only evidence that fbcon rendered the boot;
  6. runs ``poweroff`` and requires the machine to exit.

With --fastfetch-version, it also runs the embedded binary and checks that
both its reported source version and the normal command work in the shell.

The serial log supplies machine-readable evidence. Input is injected through
QMP, and command output proves it reached the shell through the selected bus.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from check_laptop_esp_log import ESP_OFFSET

ROOT = Path(__file__).resolve().parents[2]


def _a20log(image: Path, *args: str, directory: Path | None = None) -> subprocess.CompletedProcess:
    source = ["--dir", str(directory)] if directory else ["--image", str(image)]
    return subprocess.run(
        [sys.executable, str(ROOT / "tools" / "a20log"), *args, *source],
        capture_output=True, text=True, check=False)


def verify_persistent_log(image: Path, directory: Path | None = None) -> str:
    """The USB artifact must carry evidence after the machine powers off."""
    listed = _a20log(image, "list", directory=directory)
    if listed.returncode != 0:
        raise RuntimeError(f"no persistent boot log: {listed.stderr.strip()}")
    if "state=clean" not in listed.stdout:
        raise RuntimeError(
            "CURRENT was not marked clean before poweroff: "
            f"{listed.stdout.strip()}")
    markers = ("[INIT] System ready", "[SELFTEST] i2c-dw: PASS",
               "[SELFTEST] r8169: PASS")
    for marker in markers:
        shown = _a20log(image, "show", "--grep", marker, directory=directory)
        if shown.returncode != 0:
            raise RuntimeError(f"persistent log is missing {marker!r}")
    for marker in ("STAGE loader", "STAGE exit-boot-services"):
        early = _a20log(image, "show", "--boot", "loader", "--grep", marker)
        if early.returncode != 0 or marker not in early.stdout:
            raise RuntimeError(
                f"early JSONL is missing {marker!r}: {early.stderr.strip()}")
    bootlog = _a20log(image, "bootlog")
    if bootlog.returncode != 0:
        raise RuntimeError(f"no UEFI breadcrumb: {bootlog.stderr.strip()}")
    for marker in ("STAGE loader", "RESULT handoff=attempt"):
        if marker not in bootlog.stdout:
            raise RuntimeError(f"UEFI breadcrumb is missing {marker!r}")
    summary = _a20log(image, "summary", directory=directory)
    if summary.returncode != 0 or "bootcount=" not in summary.stdout:
        raise RuntimeError(
            f"CURRENT is missing bootcount: {summary.stderr.strip()}")
    return listed.stdout.strip().splitlines()[0]


class Qmp:
    def __init__(self, path: Path, timeout: float = 10.0):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(str(path))
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        self.file = self.sock.makefile('rwb')
        self._read_json()
        self.execute('qmp_capabilities')

    def _read_json(self):
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError('QMP closed')
            message = json.loads(line)
            if 'event' not in message:
                return message

    def execute(self, name: str, **arguments):
        payload = {'execute': name}
        if arguments:
            payload['arguments'] = arguments
        self.file.write((json.dumps(payload) + '\r\n').encode())
        self.file.flush()
        reply = self._read_json()
        if 'error' in reply:
            raise RuntimeError(f'QMP {name}: {reply["error"]}')
        return reply.get('return')

    def hmp(self, command: str):
        return self.execute('human-monitor-command', **{'command-line': command})

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass


KEYS = {
    ' ': 'spc', '\n': 'ret', '/': 'slash', '-': 'minus', '.': 'dot',
    ':': 'shift-semicolon', '\\': 'backslash', '&': 'shift-7', '>': 'shift-dot',
    '_': 'shift-minus', '=': 'equal', "'": 'apostrophe', '"': 'shift-apostrophe',
    '(': 'shift-9', ')': 'shift-0', '[': 'bracket_left', ']': 'bracket_right',
}


def type_text(qmp: Qmp, text: str, hold_ms=200, delay=0.4) -> None:
    for ch in text:
        # Leave a full release interval between HID reports. Short pulses
        # can be lost while a many-vCPU desktop is handling startup I/O.
        key = 'shift-' + ch.lower() if ch.isascii() and ch.isupper() else KEYS.get(ch, ch)
        reply = qmp.hmp(f'sendkey {key} {hold_ms}')
        if reply and ('invalid' in reply.lower() or 'unknown' in reply.lower()):
            raise ValueError(f'QEMU rejected character {ch!r}: {reply}')
        time.sleep(delay)


def ppm_lit_pixels(path: Path) -> tuple[int, int, int]:
    data = path.read_bytes()
    if not data.startswith(b'P6'):
        raise ValueError('screendump is not a binary PPM')
    fields = []
    offset = 2
    while len(fields) < 3:
        while offset < len(data) and data[offset:offset + 1].isspace():
            offset += 1
        if data[offset:offset + 1] == b'#':
            while offset < len(data) and data[offset] != 0x0A:
                offset += 1
            continue
        start = offset
        while offset < len(data) and not data[offset:offset + 1].isspace():
            offset += 1
        fields.append(int(data[start:offset]))
    offset += 1
    width, height, maxval = fields
    if maxval > 255:
        raise ValueError('16-bit PPM is not expected from QEMU')
    pixels = data[offset:offset + width * height * 3]
    if len(pixels) < width * height * 3:
        raise ValueError('truncated screendump')
    lit = sum(1 for i in range(0, len(pixels), 3)
              if pixels[i] or pixels[i + 1] or pixels[i + 2])
    return width, height, lit


def wait_for(log: Path, markers: tuple[str, ...], timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors='replace') if log.exists() else ''
        if all(marker in text for marker in markers):
            return text
        time.sleep(0.25)
    text = log.read_text(errors='replace') if log.exists() else ''
    missing = [marker for marker in markers if marker not in text]
    raise RuntimeError(f'timed out waiting for {missing}; log: {log}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--code', type=Path,
                        default=Path(os.environ.get('OVMF_CODE',
                                                    '/usr/share/OVMF/OVMF_CODE_4M.fd')))
    parser.add_argument('--vars', type=Path,
                        default=Path(os.environ.get('OVMF_VARS',
                                                    '/usr/share/OVMF/OVMF_VARS_4M.fd')))
    parser.add_argument('--log', type=Path,
                        default=ROOT / '.kernel-build/smoke/laptop-uefi.log')
    parser.add_argument('--timeout', type=float, default=120.0)
    parser.add_argument('--internal-esp-log', action='store_true',
                        help='require the kernel log on the internal ESP')
    parser.add_argument('--fastfetch-version',
                        help='verify the embedded fastfetch project version and run fastfetch')
    input_group = parser.add_mutually_exclusive_group()
    input_group.add_argument('--usb-keyboard', action='store_true',
                             help='test a root-port USB HID keyboard')
    input_group.add_argument('--usb-hub', action='store_true',
                             help='test a USB HID keyboard behind a powered hub')
    args = parser.parse_args()
    for path in (args.image, args.code, args.vars):
        if not path.is_file():
            parser.error(f'required firmware/artifact missing: {path}')
    qemu = shutil.which('qemu-system-x86_64')
    if not qemu:
        parser.error('qemu-system-x86_64 is required')
    args.log.parent.mkdir(parents=True, exist_ok=True)
    use_usb = args.usb_keyboard or args.usb_hub

    with tempfile.TemporaryDirectory(prefix='a20-laptop-') as temporary:
        stage = Path(temporary)
        variables = stage / 'vars.fd'
        shutil.copyfile(args.vars, variables)
        qmp_path = stage / 'qmp.sock'
        shot = stage / 'screen.ppm'
        args.log.unlink(missing_ok=True)
        command = [
            qemu, '-machine', 'q35,i8042=off' if use_usb else 'q35',
            '-cpu', 'max,pdpe1gb=off',
            '-m', '1024M', '-smp', '1', '-display', 'none',
            '-no-reboot', '-net', 'none',
            '-drive', f'if=pflash,format=raw,readonly=on,file={args.code.resolve()}',
            '-drive', f'if=pflash,format=raw,file={variables}',
            '-drive', f'file={args.image.resolve()},format=raw,if=none,id=nvme0',
            '-device', 'nvme,drive=nvme0,serial=A20OSLAPTOP',
            '-serial', f'file:{args.log}',
            '-qmp', f'unix:{qmp_path},server,nowait',
        ]
        input_marker = '[PS2] module init ok'
        if use_usb:
            command += ['-device', 'qemu-xhci,id=xhci']
            if args.usb_hub:
                command += ['-device', 'usb-hub,bus=xhci.0,port=1,ports=4,port-power=on',
                            '-device', 'usb-kbd,bus=xhci.0,port=1.3']
            else:
                command += ['-device', 'usb-kbd,bus=xhci.0']
            input_marker = '[USB-HID] keyboard ready'
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                   stderr=open(stage / 'qemu.err', 'wb'))
        failure = None
        try:
            log_path = '/var/log/a20-esp/A20LOG' if args.internal_esp_log else '/var/log/a20 '
            wait_for(args.log, ('[NVME]', input_marker,
                                f'[LOG] persistent log at {log_path}',
                                '[I2C-HID] parser self-test: PASS',
                                '[I2C-HID] protocol self-test: PASS',
                                '[I2C-HID] multitouch self-test: PASS',
                                '[SELFTEST] r8169: PASS',
                                '[SELFTEST] i2c-dw: PASS',
                                'execve mksh'), args.timeout)
            qmp = Qmp(qmp_path)
            try:
                type_text(qmp, 'echo ok\n')
                if use_usb:
                    wait_for(args.log, ('[USB-HID] key event:',), 10.0)
                if args.fastfetch_version:
                    type_text(qmp, 'fastfetch --version-raw\n')
                    wait_for(args.log, (f'\n{args.fastfetch_version}\n',), 20.0)
                    # Fastfetch ends with a terminal reset. Put the marker
                    # on its own line after that escape sequence.
                    type_text(qmp, 'fastfetch && echo && echo fetch-ok\n')
                    wait_for(args.log, ('\nfetch-ok\n',), 30.0)
                time.sleep(1.5)
                type_text(qmp, 'nvstress /dev/disk0 1200 8192\n')
                stress = wait_for(args.log, ('nvme_stress ok',), 90.0)
                if stress.count('nvme_stress ok') < 1:
                    raise RuntimeError('nvme stress produced no success marker')
                qmp.hmp(f'screendump {shot}')
                time.sleep(0.5)
                type_text(qmp, 'poweroff\n')
            finally:
                qmp.close()
            text = wait_for(args.log, ('\nok\n',
                                       'System is going down for power-off NOW'),
                            args.timeout)
            if 'KERNEL PANIC' in text:
                raise RuntimeError(f'kernel panicked; log: {args.log}')
            width, height, lit = ppm_lit_pixels(shot)
            if width < 640 or height < 480 or lit < (width * height) // 200:
                raise RuntimeError(
                    f'fbcon scanout looks blank or wrong: {width}x{height}, '
                    f'{lit} lit pixels')
            try:
                status = process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                raise RuntimeError('machine did not power off')
            if status != 0:
                raise RuntimeError(f'QEMU exited with {status}')
            log_dir = None
            if args.internal_esp_log:
                subprocess.run(['mcopy', '-s', '-i',
                                f'{args.image.resolve()}@@{ESP_OFFSET}',
                                '::/A20LOG', str(stage)], check=True)
                log_dir = stage / 'A20LOG'
            log_line = verify_persistent_log(args.image.resolve(), log_dir)
            print(f'smoke-laptop-shell: PASS; scanout {width}x{height}, '
                  f'{lit} lit pixels; persistent log: {log_line}; '
                  f'log: {args.log}')
        except Exception as error:  # noqa: BLE001 - report, then clean up
            failure = error
            qemu_error = stage / 'qemu.err'
            if qemu_error.exists() and qemu_error.stat().st_size:
                print(qemu_error.read_text(errors='replace'),
                      file=sys.stderr)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
        if failure is not None:
            print(f'smoke-laptop-uefi: FAIL: {failure}', file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
