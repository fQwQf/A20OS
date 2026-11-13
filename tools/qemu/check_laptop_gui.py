#!/usr/bin/env python3
"""Boot the laptop GUI image as a USB stick and require a rendered desktop.

The image is attached through qemu-xhci/usb-storage, which is the route the
physical artifact uses: firmware boots the ESP, and after ExitBootServices the
kernel's embedded xHCI + usb-storage stack re-enumerates the same stick and
finds the "A20OS distro" GPT partition.

The gate proves four things that a text-shell boot does not:

  * the world partition is mounted and the distro init runs;
  * labwc reaches the retained GOP framebuffer through /dev/dri/card0
    (``[DRM] present``);
  * the scanout contains a real desktop, not a blank or text frame
    (screendump colour and coverage thresholds);
  * no kernel panic occurred under the desktop's I/O load.

The serial log is still required because it is the machine-readable copy of
the kernel log on a workstation without a COM1 adapter.
"""
import argparse
import code
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from check_laptop_uefi import type_text as _type_text


def type_text(qmp, text, hold_ms=80, delay=0.18):
    # Keep a full release interval while avoiding minutes of host-side typing
    # between measured guest workloads. Every command still has a guest marker.
    return _type_text(qmp, text, hold_ms=hold_ms, delay=delay)

ROOT = Path(__file__).resolve().parents[2]


class Qmp:
    def __init__(self, path: Path, timeout: float = 15.0):
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


def ppm_stats(path: Path) -> tuple[int, int, int, int]:
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
    lit = 0
    colours = set()
    for i in range(0, len(pixels), 3):
        r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
        if r or g or b:
            lit += 1
            if len(colours) < 4096:
                colours.add((r >> 3, g >> 3, b >> 3))
    return width, height, lit, len(colours)


def wait_for(log: Path, markers: tuple[str, ...], timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors='replace') if log.exists() else ''
        if any(marker in text for marker in ('KERNEL PANIC', '[PANIC]', 'KERNEL TRAP')):
            raise RuntimeError(f'guest kernel trap or panic; log: {log}')
        if any(marker in text for marker in ('poison caller=', 'transaction poisoned',
                                              'deferred orphan cleanup failed')):
            raise RuntimeError(f'guest journal or inode cleanup failed; log: {log}')
        if 'Traceback (most recent call last):' in text:
            raise RuntimeError(f'guest Python exception; log: {log}')
        if 'IBus compositor layout default verification failed' in text:
            raise RuntimeError(f'input method vendor defaults failed; log: {log}')
        if all(marker in text for marker in markers):
            return text
        time.sleep(0.25)
    text = log.read_text(errors='replace') if log.exists() else ''
    missing = [marker for marker in markers if marker not in text]
    raise RuntimeError(f'timed out waiting for {missing}; log: {log}')


def boot_hub_esp(log: Path, qmp_path: Path, timeout: float) -> None:
    # Some OVMF builds discover the disk behind a hub only after the BDS
    # boot-index scan. Launch its ESP through the UEFI shell in that case.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors='replace') if log.exists() else ''
        if 'A20OS: UEFI: loading kernel' in text:
            return
        if 'Shell>' in text:
            qmp = Qmp(qmp_path)
            try:
                type_text(qmp, 'fs0:\\efi\\boot\\bootx64.efi\n')
            finally:
                qmp.close()
            return
        time.sleep(0.25)
    raise RuntimeError(f'no boot loader or UEFI shell prompt; log: {log}')


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
                        default=ROOT / '.kernel-build/smoke/laptop-gui.log')
    parser.add_argument('--timeout', type=float, default=600.0)
    parser.add_argument('--smp', type=int, default=4)
    parser.add_argument('--memory', default='4096M')
    parser.add_argument('--memfd', action='store_true',
                        help='back guest RAM with an on-demand memfd')
    parser.add_argument('--min-memory-mib', type=int,
                        help='check online CPU count and MemTotal in the GUI terminal')
    parser.add_argument('--wifi-selftest', action='store_true',
                        help='run the packaged rtw89 userspace self-test from the desktop')
    parser.add_argument('--wifi-dhcp-test', action='store_true',
                        help='exercise the packaged DHCP script using QEMU Ethernet')
    parser.add_argument('--wifi-internet-test', action='store_true',
                        help='also require DNS and an HTTPS request through QEMU Ethernet')
    parser.add_argument('--wifi-gui-fixture',
                        help='test-only guest fixture URL from check_wifi_gui.py')
    parser.add_argument('--usb-io-stress-fixture', help='test-only raw USB read workload URL')
    parser.add_argument('--input-method-fixture', help='test-only IBus/Rime GUI workload URL')
    parser.add_argument('--platform-fixture', help='test-only keyboard LED / browser startup workload URL')
    parser.add_argument('--platform-network-command',
                        help='test-only guest network setup; defaults to DHCP with the Wi-Fi helper')
    parser.add_argument('--power-action', choices=('shutdown', 'restart'),
                        help='require an orderly guest power transition after GUI checks')
    parser.add_argument('--shutdown-console-check', action='store_true',
                        help='capture the restored text console after the desktop exits')
    parser.add_argument('--hpet-clock-test', action='store_true',
                        help='require HPET and exercise real 32-bit counter wraps')
    parser.add_argument('--overlay-dir', type=Path,
                        help='retain explicit qcow2 overlays for failure forensics')
    parser.add_argument('--ibus-defaults-check', action='store_true',
                        help='require the compiled compositor keyboard-layout default')
    parser.add_argument('--normalized-tsc', action='store_true',
                        help='expose invariant TSC and pin KVM vCPUs for calibrated-clock validation')
    parser.add_argument('--idle-profile-seconds', type=float, default=0,
                        help='measure host QEMU CPU time after the desktop settles')
    parser.add_argument('--gdb-socket', type=Path,
                        help='enable a private GDB socket for native failure sampling')
    parser.add_argument('--lock-key-check', action='store_true',
                        help='exercise compositor lock state with real USB key presses')
    parser.add_argument('--rime-lock-key-check', action='store_true',
                        help='require separate XKB and Rime lock-key fixture phases')
    parser.add_argument('--orderly-reboot', action='store_true',
                        help='request restart through the distro init instead of reboot -f')
    parser.add_argument('--gui-restart', action='store_true',
                        help='request Restart through the XFCE session bus')
    parser.add_argument('--dual-keyboard', action='store_true',
                        help='retain the PS/2 keyboard alongside USB input')
    parser.add_argument('--toggle-ime', action='store_true',
                        help='switch an already-active IBus engine to ASCII before shell injection')
    parser.add_argument('--debug-repl', action='store_true', help='retain a diagnostic QMP prompt immediately after the desktop paints')
    parser.add_argument('--interactive-on-failure', action='store_true',
                        help='keep a failed diagnostic guest in a host Python QMP REPL')
    parser.add_argument('--touchpad-check', action='store_true',
                        help='require automatic input diagnostics and the native fuzz rule')
    parser.add_argument('--storage', choices=('usb', 'usb-hub', 'nvme'),
                        default='usb', help='connection for the distro image')
    parser.add_argument('--no-snapshot', action='store_true',
                        help='write guest changes to the image so the '
                             'persistent log partition can be inspected')
    parser.add_argument('--esp-log-image', type=Path,
                        help='internal NVMe disk with an ESP for LOG_MODE=esp')
    parser.add_argument('--usb-keyboard', action='store_true',
                        help='open the desktop terminal and execute a command via USB HID')
    args = parser.parse_args()
    for path in (args.image, args.code, args.vars):
        if not path.is_file():
            parser.error(f'required firmware/artifact missing: {path}')
    if args.esp_log_image and not args.esp_log_image.is_file():
        parser.error(f'ESP log disk missing: {args.esp_log_image}')
    if args.min_memory_mib and not args.usb_keyboard:
        parser.error('--min-memory-mib requires --usb-keyboard')
    if args.wifi_selftest and not args.usb_keyboard:
        parser.error('--wifi-selftest requires --usb-keyboard')
    if args.wifi_dhcp_test and not args.wifi_selftest:
        parser.error('--wifi-dhcp-test requires --wifi-selftest')
    if args.wifi_internet_test and not args.wifi_dhcp_test:
        parser.error('--wifi-internet-test requires --wifi-dhcp-test')
    if args.wifi_gui_fixture and not args.wifi_dhcp_test:
        parser.error('--wifi-gui-fixture requires --wifi-dhcp-test')
    if args.input_method_fixture and not args.wifi_dhcp_test:
        parser.error('--input-method-fixture requires --wifi-dhcp-test')
    if args.platform_fixture and not args.usb_keyboard:
        parser.error('--platform-fixture requires USB keyboard')
    if args.usb_io_stress_fixture and (not args.usb_keyboard or not args.wifi_dhcp_test):
        parser.error('--usb-io-stress-fixture requires USB keyboard and DHCP')
    if args.touchpad_check and not args.usb_keyboard:
        parser.error('--touchpad-check requires --usb-keyboard')
    qemu = shutil.which('qemu-system-x86_64')
    if not qemu:
        parser.error('qemu-system-x86_64 is required')
    args.log.parent.mkdir(parents=True, exist_ok=True)

    kvm = Path('/dev/kvm').exists()
    accel = ['-accel', 'kvm', '-cpu', 'host'] if kvm else ['-cpu', 'max']
    if args.normalized_tsc:
        if not kvm or len(os.sched_getaffinity(0)) < args.smp:
            parser.error('--normalized-tsc requires KVM and one available host CPU per vCPU')
        accel = ['-accel', 'kvm', '-cpu', 'host,+invtsc']
    with tempfile.TemporaryDirectory(prefix='a20-laptop-gui-') as temporary:
        stage = Path(temporary)
        variables = stage / 'vars.fd'
        shutil.copyfile(args.vars, variables)
        qmp_path = stage / 'qmp.sock'
        shot = stage / 'screen.ppm'
        args.log.unlink(missing_ok=True)
        snap = '' if args.no_snapshot else ',snapshot=on'
        def disk_source(base, tag):
            if not args.overlay_dir:
                return base.resolve(), 'raw', snap
            args.overlay_dir.mkdir(parents=True, exist_ok=True)
            overlay = args.overlay_dir / (args.log.stem + '.' + tag + '.qcow2')
            if overlay.exists():
                raise RuntimeError(f'refusing to overwrite forensic overlay {overlay}')
            subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'raw',
                            '-b', str(base.resolve()), str(overlay.resolve())], check=True)
            return overlay.resolve(), 'qcow2', ''
        root_image, root_format, root_snap = disk_source(args.image, 'root')
        if args.storage == 'nvme':
            storage = [
                '-drive', f'file={root_image},format={root_format},if=none,id=nvme0{root_snap}',
                '-device', 'nvme,drive=nvme0,serial=A20OSGUI',
            ]
        else:
            storage = ['-device', 'qemu-xhci,id=xhci']
            port = '1'
            if args.storage == 'usb-hub':
                storage += ['-device', 'usb-hub,bus=xhci.0,port=1,ports=4,port-power=on']
                port = '1.3'
            storage += [
                '-drive', f'file={root_image},format={root_format},if=none,id=usb0{root_snap}',
                '-device', f'usb-storage,drive=usb0,bus=xhci.0,port={port},bootindex=1',
            ]
        if args.usb_keyboard:
            if args.storage == 'nvme':
                storage += ['-device', 'qemu-xhci,id=xhci']
            storage += ['-device', 'usb-kbd,bus=xhci.0,port=2']
        if args.esp_log_image:
            log_image, log_format, log_snap = disk_source(args.esp_log_image, 'log')
            storage += [
                '-drive', f'file={log_image},format={log_format},if=none,id=logs{log_snap}',
                '-device', 'nvme,drive=logs,serial=A20OSLOG',
            ]
        machine = 'q35,i8042=off' if args.usb_keyboard and not args.dual_keyboard else 'q35'
        memory_backend = []
        if args.memfd:
            machine += ',memory-backend=guest-ram'
            memory_backend = ['-object', f'memory-backend-memfd,id=guest-ram,size={args.memory}']
        command = [
            qemu, '-machine', machine, *memory_backend,
            *accel, '-m', args.memory, '-smp', str(args.smp),
            *(['-gdb', f'unix:{args.gdb_socket.resolve()},server=on,wait=off']
              if args.gdb_socket else []),
            '-display', 'none', '-no-reboot',
            *(['-fw_cfg', 'name=opt/x86/cmdline,string=a20.hpet32=1']
              if args.hpet_clock_test else []),
            *(['-netdev', 'user,id=dhcp-test', '-device', 'e1000,netdev=dhcp-test']
              if args.wifi_dhcp_test or args.debug_repl or args.platform_fixture else ['-net', 'none']),
            '-drive', f'if=pflash,format=raw,readonly=on,file={args.code.resolve()}',
            '-drive', f'if=pflash,format=raw,file={variables}',
            *storage,
            '-serial', f'file:{args.log}',
            '-qmp', f'unix:{qmp_path},server,nowait',
        ]
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                   stderr=open(stage / 'qemu.err', 'wb'))
        if args.normalized_tsc:
            # This host has unequal physical TSC epochs. Pin before firmware
            # calibration; exposing invtsc must not hide host migration.
            host_cpus = sorted(os.sched_getaffinity(0))
            try:
                pin_qmp = Qmp(qmp_path, timeout=5)
                try:
                    cpus = pin_qmp.execute('query-cpus-fast')
                    assert {cpu['cpu-index'] for cpu in cpus} == set(range(args.smp))
                    for cpu in cpus:
                        os.sched_setaffinity(cpu['thread-id'], {host_cpus[cpu['cpu-index']]})
                finally:
                    pin_qmp.close()
            except Exception:
                process.terminate()
                process.wait(timeout=5)
                raise
        failure = None
        try:
            if args.storage == 'usb-hub':
                boot_hub_esp(args.log, qmp_path, args.timeout)
            # The desktop needs several minutes of software rendering on TCG.
            log_dir = '/var/log/a20-esp/A20LOG' if args.esp_log_image else '/extra/var/log/a20'
            text = wait_for(args.log, (f'[SMP] {args.smp}/', '[DISTRO]',
                                       f'[LOG] persistent log at {log_dir}',
                                       'a20: session log', '[DRM] present'),
                            args.timeout)
            if 'KERNEL PANIC' in text:
                raise RuntimeError(f'kernel panicked during desktop start; log: {args.log}')
            if args.hpet_clock_test:
                if '[TIME] clocksource=HPET' not in text or 'counter_bits=32' not in text:
                    raise RuntimeError('HPET 32-bit wrap test was not selected at boot')
            if args.ibus_defaults_check or args.input_method_fixture:
                wait_for(args.log, ('IBus compositor layout default verified',), 20)
            qmp = Qmp(qmp_path)
            try:
                # The first present can be the cursor or a partial paint.  Poll
                # the scanout until the desktop is actually covered, instead of
                # sampling one instant after the marker.
                stats = (0, 0, 0, 0)
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    qmp.hmp(f'screendump {shot}')
                    time.sleep(0.5)
                    stats = ppm_stats(shot)
                    width, height, lit, colours = stats
                    if lit >= (width * height) // 10 and colours >= 32:
                        break
                    time.sleep(2)
                if args.debug_repl:
                    code.interact(banner='Diagnostic desktop ready',
                                  local={'qmp': qmp, 'type_text': type_text,
                                         'log': args.log, 'args': args, 'time': time})
                if args.usb_keyboard:
                    # A painted wallpaper can precede the compositor's input
                    # and terminal shortcut setup, especially with 16 vCPUs.
                    time.sleep(15)
                    qmp.hmp('sendkey meta_l-ret')
                    time.sleep(10)
                    if args.toggle_ime:
                        qmp.hmp('sendkey meta_l-spc')
                        time.sleep(2)
                    type_text(qmp, 'echo a20-gui-input-ok >/dev/console\n')
                    wait_for(args.log, ('\na20-gui-input-ok\n',), 30)
                    if args.idle_profile_seconds:
                        time.sleep(20)
                        def cpu_sample():
                            fields = Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()
                            return time.monotonic(), int(fields[11]) + int(fields[12])
                        start_wall, start_ticks = cpu_sample()
                        time.sleep(args.idle_profile_seconds)
                        end_wall, end_ticks = cpu_sample()
                        cpu_seconds = (end_ticks - start_ticks) / os.sysconf('SC_CLK_TCK')
                        percent = cpu_seconds / (end_wall - start_wall) * 100 / args.smp
                        args.log.with_suffix('.idle.json').write_text(json.dumps(dict(
                            wall_seconds=end_wall-start_wall, cpu_seconds=cpu_seconds,
                            virtual_cpus=args.smp, percent_of_total_cpus=percent), indent=2)+'\n')
                        print(f'PLATFORM_IDLE: {percent:.3f}% of {args.smp} CPUs', flush=True)
                    if args.min_memory_mib:
                        type_text(qmp, 'grep -c processor /proc/cpuinfo >/dev/console\n')
                        wait_for(args.log, (f'\n{args.smp}\n',), 15)
                        type_text(qmp, 'cat /proc/meminfo >/dev/console\n')
                        output = wait_for(args.log, ('\nMemTotal:',), 15)
                        total = re.search(r'^MemTotal:\s+(\d+)\s+kB', output, re.M)
                        if not total or int(total[1]) < args.min_memory_mib * 1024:
                            raise RuntimeError('guest MemTotal below the requested minimum')
                    if args.wifi_selftest:
                        type_text(qmp, '/a20/rtw89d --self-test >/dev/console 2>&1\n')
                        wait_for(args.log, ('\nrtw89 self-test PASS\n',), 45)
                    if args.touchpad_check:
                        wait_for(args.log, ('[I2C-HID] overflow and timeout recovery self-test: PASS', '[SELFTEST] dma-window: PASS'), 10)
                        wait_for(args.log, ('[TOUCHPAD] automatic diagnostic starting',), 45)
                        type_text(qmp, 'cat /etc/udev/rules.d/90-libinput-fuzz-override.rules >/dev/console\n')
                        wait_for(args.log, ('ATTR{name}=="I2C HID touchpad"',), 15)
                    if args.wifi_dhcp_test:
                        type_text(qmp, 'udhcpc -n -q -t 3 -O 114 -i eth0 -s /a20/wifi-dhcp >/dev/console 2>&1\n')
                        wait_for(args.log, ('lease of ',), 30)
                        type_text(qmp, 'cat /run/a20-wifi-lease >/dev/console\n')
                        wait_for(args.log, ('\n10.0.2.15\n',), 15)
                        type_text(qmp, 'ip route >/dev/console\n')
                        wait_for(args.log, ('default via 10.0.2.2',), 15)
                        type_text(qmp, 'cat /etc/resolv.conf >/dev/console\n')
                        wait_for(args.log, ('nameserver 10.0.2.3',), 15)
                    if args.wifi_internet_test:
                        type_text(qmp, 'curl -fsSI --max-time 20 https://example.com >/tmp/a20-http && echo a20-internet-ok >/dev/console\n')
                        wait_for(args.log, ('\na20-internet-ok\n',), 30)
                    if args.usb_io_stress_fixture:
                        distro = re.search(r'\[DISTRO\] disk (\d+) A20OS distro', text)
                        if not distro:
                            raise RuntimeError('cannot identify USB desktop disk for stress')
                        disk = '/dev/disk' + distro[1]
                        type_text(qmp, f'curl -fsS {args.usb_io_stress_fixture} -o /tmp/usb-stress.py\n')
                        type_text(qmp, f'python3 /tmp/usb-stress.py {disk} >/dev/console 2>&1 &\n')
                        wait_for(args.log, ('USB_IO_STRESS: READY',), 30)
                        # Modifier-only keys cannot type commands, but create real
                        # asynchronous USB-HID completions during raw disk I/O.
                        until = time.monotonic() + 24
                        while time.monotonic() < until:
                            qmp.hmp('sendkey shift')
                            time.sleep(0.15)
                        time.sleep(0.2)
                        wait_for(args.log, ('USB_IO_STRESS: PASS',), 90)
                    if args.input_method_fixture:
                        type_text(qmp, f'curl -f {args.input_method_fixture} -o /tmp/i.py 2>/dev/console && echo im-loaded >/dev/console\n', hold_ms=200, delay=0.4)
                        wait_for(args.log, ('\nim-loaded\n',), 20)
                        type_text(qmp, 'python3 /tmp/i.py >/dev/console 2>&1 &\n')
                        wait_for(args.log, ('MEMFD_CHURN: PASS 3072 MiB', 'ACCEPT_FLAGS: PASS'), 150)
                        wait_for(args.log, ('INPUT_METHOD: READY type nihao',), 120)
                        type_text(qmp, 'nihao ')
                        wait_for(args.log, ('INPUT_METHOD: PASS',), 100)
                        # The GTK probe prints before its final window destroy
                        # reaches the compositor. Wait for focus to return to
                        # the terminal before typing the next shell command.
                        time.sleep(3)
                    if args.wifi_gui_fixture:
                        type_text(qmp, f'curl -fsS {args.wifi_gui_fixture} -o /tmp/wifi-test.py && echo wifi-loaded >/dev/console\n')
                        wait_for(args.log, ('\nwifi-loaded\n',), 30)
                        type_text(qmp, 'python3 /tmp/wifi-test.py >/dev/console 2>&1 &\n')
                        wait_for(args.log, ('WIFI_GUI: READY',), 45)
                        qmp.hmp(f'screendump {shot}')
                        shutil.copyfile(shot, args.log.with_suffix('.wifi.ppm'))
                        wait_for(args.log, ('WIFI_GUI: PASS',), 240)
                    if args.platform_fixture:
                        if not args.wifi_dhcp_test:
                            setup = args.platform_network_command or 'udhcpc -n -q -i eth0 -s /a20/wifi-dhcp'
                            type_text(qmp, setup + ' >/dev/console 2>&1 && echo netready >/dev/console\n')
                            wait_for(args.log, ('\nnetready\n',), 45)
                        type_text(qmp, f'curl -fsS {args.platform_fixture} -o /tmp/p.py && echo platform-loaded >/dev/console\n')
                        wait_for(args.log, ('\nplatform-loaded\n',), 30)
                        type_text(qmp, 'python3 /tmp/p.py >/dev/console 2>&1 &\n')
                        if args.lock_key_check:
                            phases = ('LOCK_XKB', 'LOCK_RIME') if args.rime_lock_key_check else ('LOCK_KEYS',)
                            for phase in phases:
                                wait_for(args.log, (phase + ': READY',), 45)
                                for key, delay in [('caps_lock', 1.2), ('caps_lock', .5),
                                                   ('num_lock', 1.2), ('num_lock', .5)]:
                                    reply = qmp.hmp(f'sendkey {key} 200')
                                    if reply and ('invalid' in reply.lower() or 'unknown' in reply.lower()):
                                        raise RuntimeError(reply)
                                    time.sleep(delay)
                                wait_for(args.log, (phase + ': PASS',), 15)
                        wait_for(args.log, ('PLATFORM: PASS',), 240)
                    if args.power_action:
                        # Browser windows can outlive their original thread.
                        # Start/raise a terminal and prove where keys land.
                        qmp.hmp('sendkey meta_l-ret')
                        time.sleep(3)
                        type_text(qmp, 'echo a20-power-ready >/dev/console\n')
                        wait_for(args.log, ('\na20-power-ready\n',), 30)
                        if args.power_action == 'shutdown':
                            command = 'dbus-send --session --type=method_call --dest=org.xfce.SessionManager /org/xfce/SessionManager org.xfce.Session.Manager.Shutdown boolean:false\n'
                        elif args.gui_restart:
                            command = 'dbus-send --session --type=method_call --dest=org.xfce.SessionManager /org/xfce/SessionManager org.xfce.Session.Manager.Restart boolean:false\n'
                        elif args.orderly_reboot:
                            command = 'reboot\n'
                        else:
                            command = 'reboot -f\n'
                        if args.power_action == 'shutdown' and args.platform_fixture:
                            command = 'sh /tmp/a20-gui-poweroff\n'
                        type_text(qmp, command[:-1])
                        power_log_start = args.log.stat().st_size
                        requested_at = time.monotonic()
                        type_text(qmp, '\n')
                        if args.shutdown_console_check:
                            deadline = time.monotonic() + 35
                            while time.monotonic() < deadline:
                                with args.log.open('rb') as power_log:
                                    power_log.seek(power_log_start)
                                    text = power_log.read().decode(errors='replace')
                                if '[DISPLAY] Desktop released display; console restored.' in text:
                                    console_shot = args.log.with_suffix('.shutdown.ppm')
                                    qmp.hmp(f'screendump {console_shot}')
                                    print('SHUTDOWN_CONSOLE: captured restored display', flush=True)
                                    break
                                if process.poll() is not None:
                                    raise RuntimeError('guest exited before restoring shutdown console')
                                time.sleep(0.02)
                            else:
                                raise RuntimeError('desktop did not restore shutdown console')
                        process.wait(timeout=45)
                        elapsed = time.monotonic() - requested_at
                        args.log.with_suffix('.power.json').write_text(json.dumps(dict(
                            action=args.power_action, seconds_from_enter=elapsed,
                            exit=process.returncode), indent=2)+'\n')
                        if process.returncode != 0:
                            raise RuntimeError(f'guest power transition exit={process.returncode}')
                        marker = '[ACPI-POWER] entering firmware S5' if args.power_action == 'shutdown' else '[ACPI-POWER] requesting FADT reset'
                        wait_for(args.log, (marker,), 5)
                        print(f'PLATFORM_POWER: PASS {args.power_action}; guest exited through ACPI', flush=True)
            finally:
                qmp.close()
            width, height, lit, colours = stats
            screenshot = args.log.with_suffix('.ppm')
            shutil.copyfile(shot, screenshot)
            if width < 640 or height < 480:
                raise RuntimeError(f'unexpected scanout {width}x{height}')
            if lit < (width * height) // 10:
                raise RuntimeError(
                    f'desktop scanout is mostly black: {lit} lit pixels')
            if colours < 32:
                raise RuntimeError(
                    f'desktop scanout has only {colours} colours; '
                    'this looks like the text console, not a rendered desktop')
            text = args.log.read_text(errors='replace')
            if args.hpet_clock_test and '[TIME] clocksource=HPET' not in text:
                raise RuntimeError('shared HPET clock was not selected')
            if args.hpet_clock_test and 'counter_bits=32' not in text:
                raise RuntimeError('32-bit HPET wrap path was not selected')
            if args.normalized_tsc and 'invariant TSC validation failed' in text:
                raise RuntimeError('calibrated clock fell back during the workload')
            failures = ('KERNEL PANIC', '[PANIC]', 'KERNEL TRAP', 'SIGSEGV:', '[HFF] fill fail', '[HFF] cache_get NULL',
                        'FATAL: pid=', '[XHCI] wait_event timeout',
                        '[EXT4] deferred orphan cleanup failed',
                        '[USB-STORAGE] incomplete block transfer', 'a20: desktop session exited')
            if any(marker in text for marker in failures):
                raise RuntimeError(f'desktop I/O or session failed; log: {args.log}')
            print(f'smoke-laptop-gui: PASS; scanout {width}x{height}, '
                  f'{lit} lit pixels, {colours} colours; log: {args.log}')
        except Exception as error:  # noqa: BLE001 - report, then clean up
            failure = error
            try:
                diagnostic_qmp = Qmp(qmp_path, timeout=2)
                diagnostic_qmp.hmp(f'screendump {shot}')
                diagnostic_qmp.close()
                shutil.copyfile(shot, args.log.with_suffix('.failure.ppm'))
            except (OSError, RuntimeError):
                pass
            qemu_error = stage / 'qemu.err'
            if qemu_error.exists() and qemu_error.stat().st_size:
                print(qemu_error.read_text(errors='replace'), file=sys.stderr)
            if args.interactive_on_failure and process.poll() is None:
                diagnostic_qmp = Qmp(qmp_path)
                try:
                    code.interact(banner=f'Diagnostic guest retained: {error}',
                                  local={'qmp': diagnostic_qmp, 'type_text': type_text,
                                         'log': args.log, 'args': args, 'time': time})
                finally:
                    diagnostic_qmp.close()
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
        if failure is not None:
            print(f'smoke-laptop-gui: FAIL: {failure}', file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
