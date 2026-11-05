#!/usr/bin/env python3
"""Verify native OVMF GOP -> fbdev -> userspace RGB24 rendering, optionally HDA.

Requires an x86 embedded RAMFS_USER=1 RAMFS_MEDIA=1 EFI image. Samples actual
QEMU display pixels via QMP screendump; serial markers alone cannot pass.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import math
import hashlib
import subprocess
import sys
import tempfile
import time
import wave
from array import array

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cli"))
from a20_resource import Policy, Requirement, gate


def ppm_read(path):
    data = path.read_bytes()
    at = 0
    values = []
    while len(values) < 4:
        while at < len(data) and data[at:at+1].isspace():
            at += 1
        if at < len(data) and data[at] == 35:
            at = data.index(b'\n', at) + 1
            continue
        start = at
        while at < len(data) and not data[at:at+1].isspace():
            at += 1
        values.append(data[start:at])
    if values[0] != b'P6' or values[3] != b'255' or at >= len(data):
        raise ValueError('unsupported QMP screenshot format')
    width, height = map(int, values[1:3])
    pixels = data[at+1:]
    if width < 320 or height < 240 or len(pixels) != width * height * 3:
        raise ValueError('invalid QMP screenshot geometry')
    return width, height, pixels


def verify_bars(path):
    width, height, pixels = ppm_read(path)
    for offset, expected in [(53, (255, 0, 0)), (160, (0, 255, 0)),
                             (266, (0, 0, 255))]:
        x, y = (width - 320)//2 + offset, (height - 240)//2 + 120
        at = (y*width+x)*3
        actual = tuple(pixels[at:at+3])
        if actual != expected:
            raise ValueError(f'actual screen pixel ({x},{y})={actual}, expected={expected}')


def verify_audio(path):
    with wave.open(str(path), 'rb') as stream:
        if stream.getnchannels() != 2 or stream.getsampwidth() != 2 or stream.getframerate() != 48000:
            raise ValueError('unexpected HDA PCM capture format')
        samples = array('h', stream.readframes(stream.getnframes()))
    if os.sys.byteorder != 'little':
        samples.byteswap()
    if not samples or max(samples) < 1000 or min(samples) > -1000:
        raise ValueError('HDA output contains no bipolar PCM tone')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--efi', type=Path, required=True)
    parser.add_argument('--code', type=Path, default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--vars', type=Path, default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    parser.add_argument('--output', type=Path, default=ROOT/'.kernel-build/smoke/uefi-media')
    parser.add_argument('--audio', action='store_true')
    parser.add_argument('--media-files', action='store_true',
                        help='boot a temporary GPT ESP containing actual clip.rgb/clip.wav files')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    for path in (args.efi, args.code, args.vars):
        if not path.is_file():
            parser.error(f'missing required artifact: {path}')
    qemu = shutil.which('qemu-system-x86_64')
    if not qemu:
        parser.error('qemu-system-x86_64 is required')
    policy = Policy.from_env()
    gate(Requirement(768 + policy.reserve_mem_mb, 1, policy.min_disk_mb), policy,
         ROOT, wait=False, label='UEFI GOP userspace media smoke')
    args.output.mkdir(parents=True, exist_ok=True)
    args.output = args.output.resolve()
    log_path, shot = args.output/'serial.log', args.output/'screen.ppm'
    audio_path = args.output/'hda.wav'
    with tempfile.TemporaryDirectory(prefix='a20-gop-') as name:
        stage = Path(name)
        boot = stage/'esp/EFI/BOOT'
        boot.mkdir(parents=True)
        shutil.copyfile(args.efi, boot/'BOOTX64.EFI')
        shutil.copyfile(args.vars, stage/'vars.fd')
        if args.media_files:
            disk = stage/'media.img'
            subprocess.run([str(ROOT/'tools/boot/mk_uefi_fat_image.sh'), str(args.efi.resolve()),
                            str(disk), '-', 'BOOTX64.EFI'], check=True)
            rgb = stage/'clip.rgb'
            with rgb.open('wb') as output:
                for frame in range(12):
                    row = bytes([255,0,0])*107 + bytes([0,255,0])*106 + bytes([0,0,255])*107
                    output.write(row*240)
            pcm = stage/'clip.wav'
            with wave.open(str(pcm), 'wb') as output:
                output.setnchannels(2)
                output.setsampwidth(2)
                output.setframerate(48000)
                output.writeframes(b''.join(struct.pack('<hh', value, value)
                    for n in range(12000)
                    for value in [int(6000*math.sin(2*math.pi*1000*n/48000))]))
            for path in (rgb, pcm):
                subprocess.run(['mcopy', '-o', '-i', str(disk)+'@@1048576',
                                str(path), '::/'+path.name], check=True)
        disk_digest = None
        if args.media_files:
            with disk.open('rb') as source:
                disk_digest = hashlib.file_digest(source, 'sha256').digest()
        qmp_path = stage/'qmp.sock'

        command = [qemu, '-machine', 'q35', '-cpu', 'max,pdpe1gb=off',
                   '-m', '768M', '-smp', '1', '-display', 'none', '-serial', 'stdio',
                   '-qmp', f'unix:{qmp_path},server=on,wait=off', '-monitor', 'none',
                   '-no-reboot', '-net', 'none',
                   '-drive', f'if=pflash,format=raw,readonly=on,file={args.code.resolve()}',
                   '-drive', f'if=pflash,format=raw,file={stage}/vars.fd',
                   ]
        if args.media_files:
            command += ['-drive', f'if=ide,format=raw,file={disk}']
        else:
            command += ['-drive', f'if=none,id=esp,format=raw,file=fat:rw:{stage}/esp',
                        '-device', 'virtio-blk-pci,drive=esp']
        if args.audio:
            command += ['-audiodev', f'wav,id=audio,path={audio_path},out.frequency=48000,out.channels=2,out.format=s16',
                        '-device', 'intel-hda', '-device', 'hda-output,audiodev=audio']
        process = None
        connection = None
        with log_path.open('wb') as log:
            try:
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log,
                                           stderr=subprocess.STDOUT)
                deadline = time.monotonic() + args.timeout
                def wait_marker(marker):
                    while time.monotonic() < deadline:
                        text = log_path.read_text(errors='replace')
                        if 'KERNEL PANIC' in text or process.poll() is not None:
                            raise ValueError(f'guest stopped before {marker!r}')
                        if marker in text:
                            return
                        time.sleep(.1)
                    raise ValueError(f'timeout waiting for {marker!r}')
                wait_marker('[init] fork=0, calling execve mksh')
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection.settimeout(5)
                connection.connect(str(qmp_path))
                control = connection.makefile('rwb', buffering=0)
                json.loads(control.readline())
                def qmp(execute, arguments=None):
                    request = {'execute': execute}
                    if arguments:
                        request['arguments'] = arguments
                    control.write((json.dumps(request)+'\n').encode())
                    while True:
                        answer = json.loads(control.readline())
                        if 'error' in answer:
                            raise ValueError(str(answer))
                        if 'return' in answer:
                            return answer['return']
                qmp('qmp_capabilities')
                if args.media_files:
                    wait_marker('[MEDIA] disk')
                    process.stdin.write(b'videoplay /media/clip.rgb 320 240 10 && echo VIDEO_DONE\n')
                else:
                    process.stdin.write(b'videoplay --test 320 240 30 && echo VIDEO_DONE\n')
                process.stdin.flush()
                wait_marker('[FB] fb_ioctl: req=0x4609')
                # Allow scanout presentation to catch up with the first flush.
                time.sleep(.3)
                qmp('screendump', {'filename': str(shot)})
                verify_bars(shot)
                wait_marker('videoplay: presented '+('12' if args.media_files else '300')+' frames')
                wait_marker('\nVIDEO_DONE\n')
                if args.audio:
                    process.stdin.write(b'audioplay /media/clip.wav && echo AUDIO_DONE\n'
                                        if args.media_files else
                                        b'audioplay --tone 1000 --duration 500 && echo AUDIO_DONE\n')
                    process.stdin.flush()
                    wait_marker('\nAUDIO_DONE\n')
                if args.media_files:
                    process.stdin.write(b'if echo forbidden > /media/probe.txt; then echo MEDIA_WRITE_BAD; else echo MEDIA_READ_ONLY; fi\n')
                    process.stdin.flush()
                    wait_marker('\nMEDIA_READ_ONLY\n')
                process.stdin.write(b'poweroff\n')
                process.stdin.flush()
                process.wait(timeout=max(1, deadline-time.monotonic()))
                if process.returncode:
                    raise ValueError(f'guest exit status {process.returncode}')
                if args.audio:
                    verify_audio(audio_path)
                if args.media_files:
                    with disk.open('rb') as source:
                        if hashlib.file_digest(source, 'sha256').digest() != disk_digest:
                            raise ValueError('guest modified its read-only media image')
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                print(f'UEFI media FAIL: {error}; log: {log_path}')
                return 1
            finally:
                if connection:
                    connection.close()
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    print(f'UEFI media PASS: '+('GPT FAT32 files, read-only media, ' if args.media_files else '')+'actual GOP pixels'+(' and HDA PCM' if args.audio else '')+f'; {args.output}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
