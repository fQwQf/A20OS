#!/usr/bin/env python3
"""Bounded, RAM-only U-Boot ELF handoff for the LS2K0300 久久派.

No device is opened by run_handoff: a20 owns the transport. The standalone CLI
also provides a read-only TFTP server for exactly the validated bundle ELF.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import ipaddress
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import stat
import time
import zlib

from a20_console import ConsoleError, SessionResult, Transcript
from mk_ls2k0300_bundle import inspect_elf

DMW = 0x9000000000000000
KERNEL_WINDOW = (0x00200000, 0x08000000)
LOW_BANK_END = 0x10000000
STACK_GUARD = 0x00200000
PROMPT = re.compile(r"(?:^|[\r\n])(?P<prompt>=>|PMON>) *\Z")
ERROR = re.compile(r"(?im)(?:^|\n)(?:\*\*\s*)?(?:Unknown command|(?:## )?Error[: ]|"
                   r"ERROR[: ]|Failed to|Bad device|Invalid ELF|No such|"
                   r"File not found|TFTP error|Retry count exceeded|"
                   r"Unable to|Couldn't find|Not a valid|No ethernet)")


class HandoffError(ValueError):
    def __init__(self, stage: str, message: str):
        super().__init__(message)
        self.stage = stage


def physical(address: int) -> int:
    if address >> 60 in (8, 9):
        return address & ((1 << 60) - 1)
    if 0 <= address < (1 << 48):
        return address
    raise ValueError(f'unsupported firmware address 0x{address:x}')


def seconds(value, default):
    if value is None:
        return default
    match = re.fullmatch(r'([0-9]+(?:\.[0-9]+)?)(ms|s|m)?', str(value))
    if not match:
        raise ValueError('invalid handoff timeout')
    return float(match[1]) * {'ms': .001, 's': 1, 'm': 60, None: 1}[match[2]]


def overlaps(a, b):
    return a[0] < b[1] and b[0] < a[1]


def covered(region, ranges):
    cursor = region[0]
    for start, end in sorted(ranges):
        if start <= cursor < end:
            cursor = end
        if cursor >= region[1]:
            return True
    return False


@dataclass(frozen=True)
class MemoryPlan:
    staging: int
    size: int
    dram: tuple[tuple[int, int], ...]
    reserved: tuple[tuple[int, int], ...]


def plan_memory(bdinfo: str, elf_size: int) -> MemoryPlan:
    """Require complete vendor bdinfo DDR/LMB evidence, then choose a hole.

    LMB covers the active firmware stack/malloc/relocation and DT reservations.
    Two extra MiB below every reservation protect nested transfer stack usage.
    Kernel allocator [2,128 MiB) must also be entirely available, not just its
    PT_LOAD destinations. No writes are allowed on incomplete firmware output.
    """
    if not 0 < elf_size <= 64 * 1024 * 1024:
        raise ValueError('ELF staging size is outside the supported 1..64 MiB range')
    dram = []
    for match in re.finditer(r'(?is)DRAM bank\s*=\s*0x[0-9a-f]+\s*'
                            r'-> start\s*=\s*(0x[0-9a-f]+)\s*'
                            r'-> size\s*=\s*(0x[0-9a-f]+)', bdinfo):
        start = physical(int(match[1], 16)); size = int(match[2], 16)
        if size <= 0 or start + size > (1 << 48):
            raise ValueError('invalid DRAM bank')
        dram.append((start, start + size))
    if not dram or not covered(KERNEL_WINDOW, dram):
        raise ValueError('bdinfo does not prove DDR covers kernel allocator [2 MiB,128 MiB)')
    tables = {}
    for table in ('memory', 'reserved'):
        count = re.search(r'\b' + table + r'\.cnt\s*=\s*(0x[0-9a-f]+|[0-9]+)', bdinfo, re.I)
        if not count:
            raise ValueError(f'bdinfo is missing LMB {table}.cnt; capture full bdinfo including lmb_dump_all')
        expected = int(count[1], 0)
        rows = []
        for row in re.finditer(r'\b' + table + r'\[(\d+)\]\s*'
                               r'\[(0x[0-9a-f]+)\s*-\s*(0x[0-9a-f]+)\]\s*,\s*'
                               r'(0x[0-9a-f]+)\s+bytes', bdinfo, re.I):
            start = physical(int(row[2], 16)); end = physical(int(row[3], 16)) + 1
            if end <= start or end - start != int(row[4], 16):
                raise ValueError(f'invalid LMB {table} interval')
            rows.append((int(row[1]), (start, end)))
        if sorted(i for i, _ in rows) != list(range(expected)):
            raise ValueError(f'incomplete or duplicate LMB {table} rows')
        tables[table] = [r for _, r in rows]
    if not tables['reserved']:
        raise ValueError('LMB reports no firmware stack reservation; cannot prove a safe RAM load')
    if not covered(KERNEL_WINDOW, tables['memory']):
        raise ValueError('LMB memory does not cover kernel allocator')
    reserved = list(tables['reserved'])
    # Keep the entire tail of each bank above a live firmware pointer. This
    # conservatively covers video buffers and firmware variants whose LMB
    # does not expose those allocations individually.
    for field in ('relocaddr', 'bd address', 'fdt_blob', 'new_fdt', 'FB base',
                  'FB copy', 'malloc base', 'irq_sp', 'sp start', 'stack ptr'):
        match = re.search(r'(?im)^\s*' + re.escape(field) + r'\s*=\s*(0x[0-9a-f]+)', bdinfo)
        if not match or int(match[1], 16) == 0:
            continue
        ptr = physical(int(match[1], 16))
        for start, end in dram:
            if start <= ptr < end:
                reserved.append((ptr, end))
    protected = [(max(0, start - STACK_GUARD), end) for start, end in reserved]
    if any(overlaps(KERNEL_WINDOW, r) for r in protected):
        raise ValueError('firmware/DT/framebuffer reservations overlap kernel allocator [2 MiB,128 MiB)')
    size = (elf_size + 0xffff) & ~0xffff
    # Deliberately stay in the documented low 256 MiB cached DDR bank.
    for start, end in sorted(dram):
        candidate = (max(start, KERNEL_WINDOW[1]) + 0xffff) & ~0xffff
        limit = min(end, LOW_BANK_END)
        while candidate + size <= limit:
            region = (candidate, candidate + size)
            conflict = next((r for r in protected if overlaps(region, r)), None)
            if conflict:
                candidate = (conflict[1] + 0xffff) & ~0xffff
                continue
            if covered(region, tables['memory']):
                return MemoryPlan(DMW + candidate, elf_size, tuple(dram), tuple(reserved))
            candidate += 0x10000
    raise ValueError('no low-DDR staging region outside kernel/firmware reservations fits kernel.elf')


class Session:
    def __init__(self, transport, transcript, on_output=None, now=time.monotonic):
        self.transport = transport
        self.transcript = transcript
        self.on_output = on_output
        self.now = now

    def receive(self, timeout):
        chunk = self.transport.read(timeout)
        self.transcript.append(chunk)
        if chunk and self.on_output:
            self.on_output(chunk.decode('utf-8', 'replace'))
        return chunk.decode('utf-8', 'replace')

    def prompt(self, timeout=15.0, interrupt=False):
        last_stop = self.now()
        deadline = self.now() + timeout
        text = ''
        while self.now() < deadline:
            text += self.receive(min(.2, max(0, deadline - self.now())))
            match = PROMPT.search(text)
            if match:
                return text, match['prompt']
            if interrupt and self.now() - last_stop >= .5:
                self.transport.write(b'c\r')
                last_stop = self.now()
        raise HandoffError('prompt', 'no complete => / PMON> prompt; power-cycle while capture is active and press c')

    def command(self, command, timeout=15.0):
        self.transport.write((command + '\r').encode('ascii'))
        text, prompt = self.prompt(timeout)
        if prompt != '=>':
            raise HandoffError('firmware', 'firmware changed away from U-Boot')
        # Remove terminal-echoed input before parsing results.
        body = text.replace('\r', '').splitlines()
        if body and body[0].strip() == command:
            body = body[1:]
        result = '\n'.join(body[:-1])
        if ERROR.search(result):
            raise HandoffError('command', f'firmware command failed: {command}')
        return result


def _safe_token(value, description, pattern=r'[A-Za-z0-9_./:-]+'):
    if not re.fullmatch(pattern, value) or '..' in value.split('/'):
        raise ValueError(f'invalid {description}')
    return value


def _reject_symlink_path(path: Path):
    absolute = path.absolute()
    if any(part.is_symlink() for part in (absolute, *absolute.parents)):
        raise ValueError(f'symlink paths are refused: {path}')


def checked_payload(bundle: Path) -> bytes:
    for path in (bundle, bundle / 'kernel.elf', bundle / 'image.json'):
        _reject_symlink_path(path)
    metadata = inspect_elf(bundle / 'kernel.elf')
    declared = json.loads((bundle / 'image.json').read_text())
    payload = (bundle / 'kernel.elf').read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != metadata['sha256'] or digest != declared.get('sha256'):
        raise ValueError('bundle ELF hash differs from image.json; re-run a20 package')
    return payload


def stage_bundle(bundle: Path, mount: Path, dry_run: bool = False) -> dict:
    """Copy only /a20/kernel.elf to an explicit mounted writable FAT volume.

    No board identity check is applied: this is ordinary SD/USB staging, not a
    UF2 bootloader upload. Directory fds plus NOFOLLOW/EXCL protect existing
    files and keep a concurrent unmount from redirecting writes onto the host.
    """
    from m0sense_deploy_uf2 import mounted_fat_volumes
    bundle, mount = Path(bundle), Path(mount)
    payload = checked_payload(bundle)
    digest = hashlib.sha256(payload).hexdigest()
    _reject_symlink_path(mount)
    root = mount.resolve(strict=True)
    flags = os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    root_fd = os.open(root, flags)
    directory_fd = None
    try:
        if root not in mounted_fat_volumes() or not root.is_dir():
            raise ValueError('destination must be the root of an actually mounted writable FAT volume')
        if os.fstat(root_fd).st_dev != root.stat().st_dev:
            raise ValueError('mount changed while opening destination')
        # The shared helper's macOS fallback enumerates all mounted volumes;
        # require a positive filesystem-type check for this ordinary USB path.
        if os.sys.platform == 'darwin':
            import plistlib
            import subprocess
            try:
                info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(root)]))
            except (subprocess.CalledProcessError, plistlib.InvalidFileException) as error:
                raise ValueError('cannot prove selected macOS volume is FAT') from error
            if (str(info.get('FilesystemType', '')).lower() not in ('msdos', 'fat', 'fat32')
                    or info.get('ReadOnlyVolume', True) or info.get('ReadOnlyMedia', True)):
                raise ValueError('selected macOS volume is not writable FAT')
        destination = root / 'a20/kernel.elf'
        result = dict(staged=False, copied=False, dry_run=dry_run,
                      sha256=digest, mount=str(root), destination=str(destination),
                      already_present=False)
        try:
            entry = os.stat('a20', dir_fd=root_fd, follow_symlinks=False)
        except FileNotFoundError:
            if dry_run:
                print(f'DRY RUN: stage validated ELF to {destination}')
                return result
            os.mkdir('a20', mode=0o755, dir_fd=root_fd)
            os.fsync(root_fd)
        else:
            if not stat.S_ISDIR(entry.st_mode):
                raise ValueError('/a20 must be a real directory, not a symlink or file')
        directory_fd = os.open('a20', flags, dir_fd=root_fd)
        try:
            existing_fd = os.open('kernel.elf', os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0)
                                  | getattr(os, 'O_NONBLOCK', 0), dir_fd=directory_fd)
        except FileNotFoundError:
            existing_fd = None
        if existing_fd is not None:
            with os.fdopen(existing_fd, 'rb') as existing:
                existing_stat = os.fstat(existing.fileno())
                if not stat.S_ISREG(existing_stat.st_mode):
                    raise ValueError('existing /a20/kernel.elf is not a regular file')
                if existing_stat.st_size != len(payload):
                    raise ValueError('different /a20/kernel.elf already exists; refusing to overwrite')
                current = hashlib.file_digest(existing, 'sha256').hexdigest()
            if current != digest:
                raise ValueError('different /a20/kernel.elf already exists; refusing to overwrite')
            result.update(staged=not dry_run, already_present=True)
            if not dry_run:
                os.fsync(directory_fd)
            print(f'ELF already staged with identical SHA-256: {destination}')
            return result
        if dry_run:
            print(f'DRY RUN: stage validated ELF to {destination}')
            return result
        fd = os.open('kernel.elf', os.O_WRONLY | os.O_CREAT | os.O_EXCL
                     | getattr(os, 'O_NOFOLLOW', 0), 0o644, dir_fd=directory_fd)
        try:
            with os.fdopen(fd, 'wb', buffering=0) as target:
                remaining = memoryview(payload)
                while remaining:
                    written = target.write(remaining)
                    if not written:
                        raise OSError('short ELF staging write')
                    remaining = remaining[written:]
                os.fsync(target.fileno())
            os.fsync(directory_fd)
        except BaseException:
            # Only this newly EXCL-created file is removed; preexisting files
            # never reach this branch and are never overwritten.
            os.unlink('kernel.elf', dir_fd=directory_fd)
            os.fsync(directory_fd)
            raise
        result.update(staged=True, copied=True)
        print(f'Staged validated ELF: {destination}; unmount normally before moving the medium')
        return result
    finally:
        if directory_fd is not None:
            os.close(directory_fd)
        os.close(root_fd)


def run_handoff(transport, inst=None, *, bundle=None, transfer='mmc',
                server_ip=None, board_ip=None, source=None, device='0:1',
                command_timeout=15.0, boot_timeout=120.0, on_output=None,
                now=time.monotonic) -> SessionResult:
    """Discover U-Boot memory, verify a transfer and boot. Never opens serial.

    Root may pass an a20 Instance; successful completion validates its boot
    markers. Failure always returns a named stage and complete Transcript.
    No transfer/jump occurs unless ELF, DDR and reservations are all proven.
    """
    transcript = Transcript()
    session = Session(transport, transcript, on_output, now)
    stage = 'bundle'
    try:
        bundle = Path(bundle or Path(__file__).resolve().parent.parent / 'build/ls2k0300/handoff')
        payload = checked_payload(bundle)
        if transfer not in ('mmc', 'usb', 'tftp'):
            raise ValueError('transfer must be mmc, usb or tftp')
        device = _safe_token(device, 'storage device', r'[0-9]+:[0-9]+')
        source = _safe_token(source or ('kernel.elf' if transfer == 'tftp' else '/a20/kernel.elf'), 'source filename')
        if transfer == 'tftp':
            if not server_ip or not board_ip:
                raise ValueError('TFTP needs --server-ip and --board-ip in the same reachable LAN')
            server_ip = str(ipaddress.IPv4Address(server_ip))
            board_ip = str(ipaddress.IPv4Address(board_ip))
        handoff_config = getattr(inst, 'handoff', None)
        command_timeout = seconds(getattr(handoff_config, 'command_timeout', None), command_timeout)
        prompt_timeout = seconds(getattr(handoff_config, 'prompt_timeout', None), max(command_timeout, 60))
        boot_timeout = seconds(getattr(getattr(inst, 'target', None), 'boot_timeout', None), boot_timeout)
        stage = 'prompt'
        # c is the documented 99pi stop key; CR completes a prompt already open.
        transport.write(b'c\r')
        _, prompt = session.prompt(prompt_timeout, interrupt=True)
        if prompt == 'PMON>':
            # h is PMON's read-only command help. Never send U-Boot loaders.
            transport.write(b'h\r')
            session.prompt(command_timeout)
            raise HandoffError('pmon', 'PMON detected. Preserve this log and run h load, h g, devls, set (no arguments); see docs/platforms/2k0300.md#pmon')
        stage = 'firmware'
        version = session.command('version', command_timeout)
        if 'U-Boot' not in version:
            raise ValueError('version output does not identify U-Boot')
        bdinfo = session.command('bdinfo', command_timeout)
        boothelp = session.command('help bootelf', command_timeout)
        if 'bootelf' not in boothelp or '-p' not in boothelp:
            raise ValueError('bootelf -p is not advertised by this firmware')
        for name in ('crc32', 'base', 'load' if transfer != 'tftp' else 'tftpboot'):
            helptext = session.command('help ' + name, command_timeout)
            if name not in helptext:
                raise ValueError(f'{name} is unavailable')
        stage = 'memory'
        plan = plan_memory(bdinfo, len(payload))
        host_note = (f'\n[HOST] LS2K0300 proven staging=0x{plan.staging:x} '
                     f'bytes=0x{len(payload):x} crc32={zlib.crc32(payload):08x}\n')
        transcript.append(host_note.encode())
        if on_output:
            on_output(host_note)
        stage = 'load'
        session.command('base 0', command_timeout)
        session.command('setenv filesize', command_timeout)
        if transfer == 'tftp':
            session.command('setenv serverip ' + server_ip, command_timeout)
            session.command('setenv ipaddr ' + board_ip, command_timeout)
            network = session.command('printenv serverip ipaddr', command_timeout)
            if f'serverip={server_ip}' not in network or f'ipaddr={board_ip}' not in network:
                raise ValueError('network environment read-back failed')
            loaded = session.command(f'tftpboot {plan.staging:x} {source}', max(command_timeout, 120.0))
            if not re.search(r'Bytes transferred\s*=\s*' + str(len(payload)) + r'\b', loaded):
                raise ValueError('TFTP did not report the expected byte count')
        else:
            if transfer == 'usb':
                session.command('usb start', max(command_timeout, 60.0))
            # Probe size before writing RAM, and cap load length independently.
            session.command(f'size {transfer} {device} {source}', command_timeout)
            size = session.command('printenv filesize', command_timeout)
            match = re.search(r'(?im)^filesize=([0-9a-f]+)\s*$', size)
            if not match or int(match[1], 16) != len(payload):
                raise ValueError('storage source size differs from the packaged ELF')
            session.command('setenv filesize', command_timeout)
            loaded = session.command(f'load {transfer} {device} {plan.staging:x} {source} {len(payload):x}', max(command_timeout, 60.0))
            if not re.search(r'\b' + str(len(payload)) + r' bytes read\b', loaded):
                raise ValueError('filesystem load did not report the expected byte count')
        size = session.command('printenv filesize', command_timeout)
        match = re.search(r'(?im)^filesize=([0-9a-f]+)\s*$', size)
        if not match or int(match[1], 16) != len(payload):
            raise ValueError('filesize after transfer differs from the packaged ELF')
        stage = 'crc'
        result = session.command(f'crc32 {plan.staging:x} {len(payload):x}', command_timeout)
        matches = re.findall(r'==>\s*([0-9a-f]{8})\b', result, re.I)
        if len(matches) != 1 or int(matches[0], 16) != zlib.crc32(payload):
            raise ValueError('board CRC32 differs from the packaged ELF; boot suppressed')
        stage = 'boot'
        # Runtime env only; ensure bootelf does not silently stop after loading.
        session.command('setenv autostart yes', command_timeout)
        transport.write(f'bootelf -p {plan.staging:x}\r'.encode('ascii'))
        expected = tuple(getattr(getattr(inst, 'target', None), 'console_check', ()) or
                         ('[LS2K0300] stage=polled', 'System ready'))
        deadline = now() + boot_timeout
        bootlog = ''
        while now() < deadline:
            bootlog += session.receive(min(.2, max(0, deadline - now())))
            if all(marker in bootlog for marker in expected):
                return SessionResult(True, 'ls2k0300:boot', seen=expected, transcript=transcript)
            if PROMPT.search(bootlog) or ERROR.search(bootlog):
                raise ValueError('bootelf returned to firmware or reported an error before kernel markers')
        return SessionResult(False, 'ls2k0300:boot',
                             missing=tuple(p for p in expected if p not in bootlog), transcript=transcript)
    except KeyboardInterrupt:
        return SessionResult(False, 'ls2k0300:' + stage,
                             missing=('interrupted by user',), transcript=transcript)
    except (OSError, ValueError, struct.error, ConsoleError) as error:
        failed_stage = error.stage if isinstance(error, HandoffError) else stage
        return SessionResult(False, 'ls2k0300:' + failed_stage,
                             missing=(str(error),), transcript=transcript)


def serve_tftp(bundle: Path, bind='0.0.0.0', port=69, *, once=False, on_ready=None):
    """Minimal read-only RFC1350 server; never serves arbitrary host paths.

    Option negotiation is intentionally declined by answering DATA block 1;
    U-Boot falls back to the mandatory 512-byte TFTP blocks. The immutable ELF
    is validated once, so TFTP cannot accidentally serve a different file.
    """
    elf = bundle / 'kernel.elf'
    metadata = inspect_elf(elf)
    if metadata['sha256'] != json.loads((bundle / 'image.json').read_text()).get('sha256'):
        raise ValueError('bundle hash mismatch')
    data = elf.read_bytes()
    _serve_bytes(data, 'kernel.elf', bind, port, once=once, on_ready=on_ready)


def serve_firmware(file: Path, sha256: str, bind='0.0.0.0', port=69):
    # Separate, explicit manual migration helper: serves bytes only. It never
    # sends fload or opens a serial port. It is not used by run_handoff/a20.
    if file.name != 'u-boot-spl-gz.bin':
        raise ValueError('manual firmware server accepts only u-boot-spl-gz.bin')
    data = file.read_bytes()
    if not 0 < len(data) <= 8 * 1024 * 1024:
        raise ValueError('firmware file must fit the supported 8 MiB SPI envelope')
    digest = hashlib.sha256(data).hexdigest()
    if not re.fullmatch(r'[0-9a-fA-F]{64}', sha256) or digest != sha256.lower():
        raise ValueError('firmware SHA-256 differs from the separately recorded expected value')
    print('Manual firmware migration server only; board/model/source validation remains required.', flush=True)
    _serve_bytes(data, file.name, bind, port)


def _serve_bytes(data, filename, bind, port, *, once=False, on_ready=None):
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind((bind, port))
    if on_ready:
        on_ready(listener.getsockname())
    print(f'TFTP serving only {filename} ({len(data)} bytes) at {bind}:{listener.getsockname()[1]}', flush=True)
    try:
        while True:
            request, peer = listener.recvfrom(4096)
            fields = request[2:].split(b'\0')
            if request[:2] != b'\x00\x01' or len(fields) < 3:
                listener.sendto(b'\x00\x05\x00\x04Read requests only\0', peer)
                continue
            if fields[0] not in (filename.encode(), ('/' + filename).encode()) or fields[1].lower() != b'octet':
                listener.sendto(b'\x00\x05\x00\x01Only the selected file in octet mode\0', peer)
                continue
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as transfer:
                transfer.settimeout(2)
                transfer.bind((bind, 0))
                for offset in range(0, len(data) + 1, 512):
                    block = (offset // 512 + 1) & 0xffff
                    packet = struct.pack('!HH', 3, block) + data[offset:offset + 512]
                    acknowledged = False
                    for _ in range(5):
                        transfer.sendto(packet, peer)
                        try:
                            ack, sender = transfer.recvfrom(4096)
                        except socket.timeout:
                            continue
                        if sender == peer and ack == struct.pack('!HH', 4, block):
                            acknowledged = True
                            break
                    if not acknowledged:
                        print(f'TFTP timeout for {peer}', flush=True)
                        break
                else:
                    print(f'TFTP {filename} complete for {peer}', flush=True)
            if once:
                return
    finally:
        listener.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    server = sub.add_parser('serve-tftp', help='serve exactly the validated ELF; no writes')
    server.add_argument('--bundle', type=Path, default=Path('build/ls2k0300/handoff'))
    server.add_argument('--bind', default='0.0.0.0')
    server.add_argument('--port', type=int, default=69)
    firmware = sub.add_parser('serve-firmware', help='explicit read-only server for a manually chosen PMON migration image; never flashes')
    firmware.add_argument('--file', type=Path, required=True)
    firmware.add_argument('--sha256', required=True, help='independently recorded expected firmware hash')
    firmware.add_argument('--bind', required=True)
    firmware.add_argument('--port', type=int, default=69)
    planner = sub.add_parser('plan', help='check a captured bdinfo/log without connecting hardware')
    planner.add_argument('--bundle', type=Path, default=Path('build/ls2k0300/handoff'))
    planner.add_argument('--bdinfo', type=Path, required=True)
    staging = sub.add_parser('stage', help='copy validated ELF to an explicitly selected mounted FAT SD/USB volume')
    staging.add_argument('--bundle', type=Path, default=Path('build/ls2k0300/handoff'))
    staging.add_argument('--mount', type=Path, required=True)
    staging.add_argument('--dry-run', action='store_true')
    runner = sub.add_parser('boot', help='standalone serial RAM handoff')
    runner.add_argument('--serial', required=True)
    runner.add_argument('--baud', type=int, default=115200)
    runner.add_argument('--bundle', type=Path, default=Path('build/ls2k0300/handoff'))
    runner.add_argument('--transfer', choices=('mmc', 'usb', 'tftp'), default='mmc')
    runner.add_argument('--server-ip')
    runner.add_argument('--board-ip')
    runner.add_argument('--device', default='0:1')
    runner.add_argument('--source')
    runner.add_argument('--log', type=Path, default=Path('build/ls2k0300/handoff/serial.log'))
    args = parser.parse_args()
    try:
        if args.action == 'stage':
            print(json.dumps(stage_bundle(args.bundle, args.mount, args.dry_run), indent=2))
            return 0
        if args.action == 'serve-tftp':
            serve_tftp(args.bundle, args.bind, args.port)
            return 0
        if args.action == 'serve-firmware':
            serve_firmware(args.file, args.sha256, args.bind, args.port)
            return 0
        if args.action == 'plan':
            plan = plan_memory(args.bdinfo.read_text(), (args.bundle / 'kernel.elf').stat().st_size)
            print(json.dumps(dict(staging=hex(plan.staging), size=plan.size, dram=plan.dram, reserved=plan.reserved), indent=2))
            return 0
        from a20_console import SerialTransport
        transport = SerialTransport(args.serial, args.baud)
        try:
            result = run_handoff(transport, bundle=args.bundle, transfer=args.transfer,
                                 server_ip=args.server_ip, board_ip=args.board_ip,
                                 source=args.source, device=args.device,
                                 on_output=lambda text: print(text, end='', flush=True))
        finally:
            transport.close()
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text(result.transcript.text)
        print(f'\n{result.stage}: {"PASS" if result.ok else "; ".join(result.missing)}')
        return 0 if result.ok else 1
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
