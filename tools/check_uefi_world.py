#!/usr/bin/env python3
"""Verify a packaged UEFI world through GOP, AHCI and an xHCI USB terminal.

No virtio GPU, serial command injection, shared build or physical board is used.
The OVMF fixture fixes X-PciMmio64Mb=0: this verifies the current below-4-GiB
PCI window policy, not arbitrary firmware BAR placement or Intel modesetting.
The original disk is opened with snapshot=on and the original VARS is copied.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import secrets
import shutil
import socket
import subprocess
import tempfile
import time

from a20_resource import Policy, Requirement, gate

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_BOOT = ('[DISTRO] disk', '[INIT] Chrooted to /extra',
                 'renderer=pixman', '[DRM] present')
FATAL = re.compile(r'KERNEL PANIC|assertion failed|UBSAN:|SIGSEGV|segmentation fault|page fault|ADE/ALE:', re.I)
BOOT_MIN_SECONDS = 45
DESKTOP_SETTLE_SECONDS = 8
KEY_HOLD_MS = 100
KEY_INTERVAL_SECONDS = .2
ARTIFACTS = ('serial.log', 'qemu.log', 'desktop.ppm', 'terminal.ppm',
             'command.json', 'result.json')


def exact_marker(text: str, marker: str) -> bool:
    """A typed/echoed command containing the marker is insufficient."""
    return marker in text.replace('\r\n', '\n').split('\n')


def validate_output(text: str, status: int, marker: str) -> None:
    missing = [item for item in REQUIRED_BOOT if item not in text]
    if status != 0 or missing:
        raise ValueError(f'guest failed: exit={status}, missing={missing}')
    if FATAL.search(text):
        raise ValueError('guest contains a kernel or process failure')
    if not exact_marker(text, marker):
        raise ValueError('GUI terminal command did not produce its standalone marker')
    # Require the kernel shutdown path, not a message printed only by A20's
    # recovery poweroff utility. The apk world uses BusyBox's reboot syscall.
    if '[SD] POWER_OFF entry' not in text:
        raise ValueError('guest did not take its power-off path')
    audits = re.findall(r'\[SD\]\s*audit errors=(\d+)\b', text)
    if not audits or any(int(value) for value in audits):
        raise ValueError(f'missing or failed shutdown audit: {audits}')


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    """Read QMP's binary P6 screenshot without interpreting pixel whitespace."""
    data = path.read_bytes()
    offset = 0
    tokens = []
    for _ in range(4):
        while offset < len(data):
            if data[offset] in b' \t\r\n':
                offset += 1
            elif data[offset] == 35:
                newline = data.find(b'\n', offset)
                if newline < 0:
                    raise ValueError('unterminated PPM comment')
                offset = newline + 1
            else:
                break
        start = offset
        while offset < len(data) and data[offset] not in b' \t\r\n':
            offset += 1
        tokens.append(data[start:offset])
    if tokens[0] != b'P6' or tokens[3] != b'255' or offset >= len(data):
        raise ValueError('unsupported QMP screenshot format')
    try:
        width, height = int(tokens[1]), int(tokens[2])
    except ValueError as error:
        raise ValueError('invalid screenshot dimensions') from error
    # Consume one header separator; subsequent whitespace belongs to pixels.
    if data[offset:offset + 2] == b'\r\n':
        offset += 2
    else:
        offset += 1
    pixels = data[offset:]
    if not (320 <= width <= 16384 and 240 <= height <= 16384) or len(pixels) != width * height * 3:
        raise ValueError('invalid screenshot geometry or incomplete pixels')
    return width, height, pixels


def compare_screens(before: Path, after: Path) -> dict[str, int]:
    width, height, original = read_ppm(before)
    other_width, other_height, updated = read_ppm(after)
    if (width, height) != (other_width, other_height):
        raise ValueError('display mode changed during terminal verification')
    changed = sum(original[index:index + 3] != updated[index:index + 3]
                  for index in range(0, len(original), 3))
    # Clock/caret movement cannot stand in for a newly opened terminal window.
    minimum = max(128, width * height // 100)
    if changed < minimum:
        raise ValueError(f'no substantial terminal presentation: {changed} changed pixels, need {minimum}')
    return {'width': width, 'height': height, 'changed_pixels': changed,
            'minimum_changed_pixels': minimum}


def key_codes(char: str) -> tuple[str, ...]:
    if char in 'abcdefghijklmnopqrstuvwxyz0123456789':
        return (char,)
    mapping = {' ': ('spc',), '-': ('minus',), '/': ('slash',),
               '>': ('shift', 'dot')}
    try:
        return mapping[char]
    except KeyError as error:
        raise ValueError(f'unsupported smoke-command key: {char!r}') from error


def kvm_available() -> bool:
    if platform.machine().lower() not in ('x86_64', 'amd64'):
        return False
    try:
        descriptor = os.open('/dev/kvm', os.O_RDWR | os.O_CLOEXEC)
        try:
            if fcntl.ioctl(descriptor, 0xAE00, 0) != 12:  # KVM_GET_API_VERSION
                return False
            vm = fcntl.ioctl(descriptor, 0xAE01, 0)  # KVM_CREATE_VM
            os.close(vm)
            return True
        finally:
            os.close(descriptor)
    except OSError:
        return False


def drive_path(path: Path) -> str:
    # QEMU's key/value option parser escapes a literal comma by doubling it.
    return str(path).replace(',', ',,')


def qemu_command(qemu: str, image: Path, code: Path, variables: Path,
                 qmp_path: Path, serial: Path, acceleration: str) -> list[str]:
    cpu = ('host' if acceleration == 'kvm' else 'max') + ',pdpe1gb=off,avx=off,avx2=off'
    return [qemu, '-machine', 'q35', '-accel', acceleration, '-cpu', cpu,
            '-m', '2048M', '-smp', '1', '-display', 'none',
            '-serial', f'file:{serial}', '-monitor', 'none', '-no-reboot', '-nic', 'none',
            '-fw_cfg', 'name=opt/ovmf/X-PciMmio64Mb,string=0',
            '-drive', f'if=pflash,format=raw,readonly=on,file={drive_path(code)}',
            '-drive', f'if=pflash,format=raw,file={drive_path(variables)}',
            '-drive', f'if=none,id=world,format=raw,snapshot=on,file={drive_path(image)}',
            '-device', 'ide-hd,drive=world,bus=ide.0',
            '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0',
            '-device', 'usb-mouse,bus=xhci.0', '-device', 'intel-hda', '-device', 'hda-duplex',
            '-qmp', f'unix:{qmp_path},server=on,wait=off']


class QMP:
    def __init__(self, path: Path, deadline: float):
        self.deadline = deadline
        self.connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.stream = None
        self.sequence = 0
        try:
            self.connection.settimeout(min(10, max(.01, deadline - time.monotonic())))
            self.connection.connect(str(path))
            self.stream = self.connection.makefile('rwb', buffering=0)
            greeting = self.receive()
            if 'QMP' not in greeting:
                raise ValueError('invalid QMP greeting')
            self.execute('qmp_capabilities')
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.stream is not None:
            self.stream.close()
        self.connection.close()

    def receive(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('QMP overall timeout')
        self.connection.settimeout(min(10, remaining))
        line = self.stream.readline(2 * 1024 * 1024)
        if not line:
            raise ValueError('unexpected QMP EOF')
        if not line.endswith(b'\n'):
            raise ValueError('incomplete or oversized QMP reply')
        answer = json.loads(line)
        if not isinstance(answer, dict):
            raise ValueError('QMP reply is not an object')
        return answer

    def execute(self, command: str, arguments=None):
        self.sequence += 1
        request = {'execute': command, 'id': self.sequence}
        if arguments is not None:
            request['arguments'] = arguments
        self.connection.sendall((json.dumps(request) + '\n').encode())
        while True:
            answer = self.receive()
            if 'event' in answer:
                continue
            if answer.get('id') != self.sequence:
                raise ValueError('unexpected QMP response ID')
            if 'error' in answer:
                raise ValueError(f'QMP {command}: {answer["error"]}')
            if 'return' not in answer:
                raise ValueError(f'invalid QMP response: {answer}')
            return answer['return']

    def send_key(self, codes):
        self.execute('send-key', {'keys': [{'type': 'qcode', 'data': code} for code in codes],
                                  'hold-time': KEY_HOLD_MS})


def digest(path: Path) -> str:
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / '.kernel-build/smoke/uefi-world')
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--code', type=Path, default=Path(os.environ.get('OVMF_CODE', '/usr/share/OVMF/OVMF_CODE_4M.fd')))
    parser.add_argument('--vars', type=Path, default=Path(os.environ.get('OVMF_VARS', '/usr/share/OVMF/OVMF_VARS_4M.fd')))
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('--timeout must be finite and positive')
    for name in ('image', 'code', 'vars'):
        path = getattr(args, name).resolve()
        if not path.is_file():
            parser.error(f'missing {name} artifact: {path}')
        setattr(args, name, path)
    qemu = shutil.which('qemu-system-x86_64')
    if not qemu:
        parser.error('qemu-system-x86_64 is required')
    args.output = args.output.resolve()
    for artifact in ARTIFACTS:
        if (args.output / artifact).resolve() in (args.image, args.code, args.vars):
            parser.error(f'output artifact aliases an input: {artifact}')
    policy = Policy.from_env()
    gate(Requirement(2048 + policy.reserve_mem_mb, 1, policy.min_disk_mb), policy,
         ROOT, wait=False, label='UEFI GOP world USB terminal smoke')
    args.output.mkdir(parents=True, exist_ok=True)
    for artifact in ARTIFACTS:
        (args.output / artifact).unlink(missing_ok=True)
    serial, qemu_log = args.output / 'serial.log', args.output / 'qemu.log'
    desktop, terminal = args.output / 'desktop.ppm', args.output / 'terminal.ppm'
    image_hash, vars_hash = digest(args.image), digest(args.vars)
    acceleration = 'kvm' if kvm_available() else 'tcg'
    marker = 'a20-uefi-world-' + secrets.token_hex(8)
    result = {'passed': False, 'image': str(args.image), 'marker': marker,
              'acceleration': acceleration, 'ram_mb': 2048, 'cpus': 1,
              'storage': 'AHCI', 'display': 'GOP', 'keyboard': 'xHCI USB',
              'key_hold_ms': KEY_HOLD_MS, 'key_interval_ms': int(KEY_INTERVAL_SECONDS * 1000),
              'ovmf_mmio64_mb': 0, 'image_sha256_before': image_hash,
              'vars_sha256_before': vars_hash}
    process = control = None
    status = 1
    try:
        with tempfile.TemporaryDirectory(prefix='a20-uefi-world-') as temporary:
            stage = Path(temporary)
            variables, qmp_path = stage / 'vars.fd', stage / 'qmp.sock'
            shutil.copyfile(args.vars, variables)
            command = qemu_command(qemu, args.image, args.code, variables,
                                   qmp_path, serial, acceleration)
            (args.output / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
            with qemu_log.open('wb') as log:
                try:
                    started = time.monotonic()
                    deadline = started + args.timeout
                    process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                               stdout=log, stderr=subprocess.STDOUT)

                    def log_text():
                        return serial.read_text(errors='replace') if serial.exists() else ''

                    def alive():
                        if time.monotonic() >= deadline:
                            raise TimeoutError('UEFI world overall timeout')
                        if process.poll() is not None:
                            raise ValueError(f'guest exited early: {process.returncode}')
                        if FATAL.search(log_text()):
                            raise ValueError('guest contains a kernel or process failure')

                    def pause(seconds):
                        until = time.monotonic() + seconds
                        while True:
                            remaining = until - time.monotonic()
                            if remaining <= 0:
                                break
                            alive()
                            time.sleep(min(.1, remaining))

                    while not qmp_path.exists():
                        alive()
                        time.sleep(.1)
                    control = QMP(qmp_path, deadline)
                    ready_since = None
                    while True:
                        alive()
                        now = time.monotonic()
                        if all(item in log_text() for item in REQUIRED_BOOT):
                            if ready_since is None:
                                ready_since = now
                            if now - ready_since >= DESKTOP_SETTLE_SECONDS and now - started >= BOOT_MIN_SECONDS:
                                break
                        else:
                            ready_since = None
                        time.sleep(.2)
                    print('UEFI world: GOP desktop ready; opening USB terminal', flush=True)
                    control.execute('screendump', {'filename': str(desktop)})
                    control.send_key(('meta_l', 'ret'))
                    pause(12)

                    def type_command(text):
                        for char in text:
                            alive()
                            control.send_key(key_codes(char))
                            pause(KEY_INTERVAL_SECONDS)
                        control.send_key(('ret',))

                    type_command('echo ' + marker + ' >/dev/console')
                    while not exact_marker(log_text(), marker):
                        alive()
                        time.sleep(.1)
                    pause(1)
                    control.execute('screendump', {'filename': str(terminal)})
                    result['pixels'] = compare_screens(desktop, terminal)
                    print('UEFI world: USB terminal executed marker; requesting poweroff', flush=True)
                    # BusyBox's plain poweroff signals an init implementation;
                    # this world uses its own stage-2 supervisor. -f calls the
                    # kernel reboot syscall, whose orderly path still syncs
                    # filesystems and performs the shutdown audit before S5.
                    type_command('poweroff -f')
                    process.wait(timeout=max(.01, deadline - time.monotonic()))
                    result['qemu_exit_status'] = process.returncode
                    validate_output(log_text(), process.returncode, marker)
                    status = 0
                finally:
                    if control is not None:
                        control.close()
                    if process is not None and process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
    except KeyboardInterrupt:
        result['error'] = 'interrupted'
        status = 130
    except (OSError, ValueError, TimeoutError, subprocess.SubprocessError) as error:
        result['error'] = str(error)
        status = 1
    finally:
        try:
            result['image_sha256_after'] = digest(args.image)
            result['vars_sha256_after'] = digest(args.vars)
            if result['image_sha256_after'] != image_hash or result['vars_sha256_after'] != vars_hash:
                result['error'] = 'original disk image or firmware VARS changed'
                status = 1
        except OSError as error:
            result['error'] = f'cannot verify original artifacts: {error}'
            status = 1
        result['passed'] = status == 0
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if status:
        print(f'UEFI world FAIL: {result.get("error", "verification failed")}; artifacts: {args.output}')
    else:
        print(f'UEFI world PASS: actual GOP changes, GUI USB shell, clean S5 audit; artifacts: {args.output}')
    return status


if __name__ == '__main__':
    raise SystemExit(main())
