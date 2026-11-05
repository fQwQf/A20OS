#!/usr/bin/env python3
"""Boot the real XFCE desktop image with the BSD backend and verify curl
HTTP/HTTPS from inside its labwc session. Uses an image copy only."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', type=Path, required=True)
parser.add_argument('--fat', type=Path, required=True)
parser.add_argument('--world', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cpus', default='4,6')
parser.add_argument('--smp', default='2')
parser.add_argument('--timeout', type=int, default=180)
parser.add_argument('--browser', action='store_true',
                    help='run Firefox/Marionette HTTP+HTTPS quick gate')
parser.add_argument('--firefox-probe', action='store_true',
                    help='diagnose Firefox process startup only')
parser.add_argument('--gdb-port', type=int,
                    help='expose a QEMU gdbstub on this host TCP port')
parser.add_argument('--no-append', action='store_true',
                    help='omit -append and use the board defaults')
parser.add_argument('--append-args',
                    help='override the -append string used for the test')
parser.add_argument('--monitor-socket', type=Path,
                    help='expose a QEMU HMP monitor on this unix socket')
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('net_kvm', root/'tools/bench/bench-net-kvm.py')
net = importlib.util.module_from_spec(spec)
spec.loader.exec_module(net)
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
kernel, fat, world = args.kernel.resolve(), args.fat.resolve(), args.world.resolve()
if args.firefox_probe:
    guest_script = root/'tools/tests/bsd-firefox-probe.sh'
    guest_path = '/usr/lib/a20/bsd-firefox-probe.sh'
    done_marker = 'BSD_FFPROBE_WRAPPER_DONE'
elif args.browser:
    guest_script = root/'tools/tests/bsd-browser.sh'
    guest_path = '/usr/lib/a20/bsd-browser.sh'
    done_marker = 'BSD_BROWSER_DONE'
else:
    guest_script = root/'tools/tests/bsd-desktop-net.sh'
    guest_path = '/usr/lib/a20/bsd-desktop-net.sh'
    done_marker = 'BSD_DESKTOP_NET_DONE'
image = out/'world.img'
subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(world), str(image)], check=True)
autostart = out/'autostart'
autostart.write_text(
    f'sh {guest_path} >/dev/console 2>&1 &\n' +
    (root/'packages/overlay/xfce/root/.config/labwc/autostart').read_text())
net.install(image, guest_script, guest_path)
net.install(image, root/'tools/tests/bsd_socket_loopback.py',
            '/usr/lib/a20/bsd_socket_loopback.py')
net.install(image, root/'tools/tests/bsd_net_reconfig.sh',
            '/usr/lib/a20/bsd_net_reconfig.sh')
net.install(image, autostart, '/root/.config/labwc/autostart')
if args.browser:
    net.install(image, root/'tools/tests/xfce-firefox-webdriver.py',
                '/usr/lib/a20/xfce-firefox-webdriver.py')
if args.firefox_probe:
    net.install(image, root/'tools/tests/bsd_firefox_probe.py',
                '/usr/lib/a20/bsd_firefox_probe.py')
argv = ['taskset', '-c', args.cpus, 'qemu-system-x86_64', '-machine', 'q35',
        '-accel', 'kvm', '-cpu', 'host', '-m', '4G', '-smp', args.smp,
        '-display', 'none', '-vga', 'none', '-device', 'virtio-gpu-pci',
        '-device', 'virtio-keyboard-pci', '-device', 'virtio-mouse-pci',
        *(['-monitor', 'none'] if not args.monitor_socket else []), '-serial', 'stdio', '-snapshot', '-no-reboot',
        '-kernel', str(kernel),
        '-drive', f'file={fat},if=none,format=raw,id=x0',
        '-device', 'virtio-blk-pci,drive=x0',
        '-drive', f'file={image},if=none,format=raw,id=xworld',
        '-device', 'virtio-blk-pci,drive=xworld', '-netdev', 'user,id=net',
        '-device', net.NIC, '-object', f'filter-dump,id=pcap,netdev=net,file={out/"bsd.pcap"}',
        ]
if not args.no_append:
    argv += ['-append', args.append_args or
             'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 '
             'a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.dhcp=0']
if args.gdb_port:
    argv += ['-gdb', f'tcp::{args.gdb_port}']
if args.monitor_socket:
    argv += ['-monitor', f'unix:{args.monitor_socket},server=on,wait=off']
def required_markers(arguments):
    if arguments.firefox_probe:
        return ['backend: bsd', 'FFPROBE_DONE', 'BSD_FFPROBE_WRAPPER_RC 0']
    if arguments.browser:
        return ['backend: bsd', 'BSD_BROWSER_CURL_RC 0',
                'BSD_BROWSER_WEBDRIVER_RC 0', 'BSD_BROWSER_PASS']
    return ['backend: bsd', 'BSD_RECONFIG_MTU_1400_RC 0',
            'BSD_RECONFIG_ADDR_ALT_RC 0', 'BSD_RECONFIG_ADDR_RESTORE_RC 0',
            'BSD_RECONFIG_MTU_1500_RC 0', 'BSD_RECONFIG_ROUTE_DEL_RC 0', 'BSD_RECONFIG_ROUTE_ADD_RC 0', 'BSD_RECONFIG_HTTPS_RC 0',
            'BSD_RECONFIG_CONTENT 0', 'BSD_DESKTOP_HTTP_RC 0',
            'BSD_DESKTOP_HTTP_CONTENT 0', 'BSD_DESKTOP_HTTPS_RC 0',
            'BSD_DESKTOP_HTTPS_CONTENT 0', 'BSD_DESKTOP_NET_PASS']


manifest = dict(argv=argv, passed=False,
                kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
                world_sha256=hashlib.sha256(world.read_bytes()).hexdigest(),
                script_sha256=hashlib.sha256(guest_script.read_bytes()).hexdigest())
guest = None
try:
    guest = net.Guest(argv, out/'serial.log', ready_pattern=None)
    raw, _ = guest.until((done_marker + r'\r*\n').encode(), args.timeout)
    text = raw.decode(errors='replace').replace('\r', '')
    (out/'results.txt').write_text(text)
    required = required_markers(args)
    manifest['passed'] = all(marker in text for marker in required)
    if not manifest['passed']:
        print(text[-14000:])
except Exception as error:
    manifest['error'] = str(error)
    # poweroff may close the serial PTY in the same read that carried the final
    # marker; evaluate the persisted log before declaring failure.
    log = out/'serial.log'
    if log.exists():
        text = log.read_text(errors='replace').replace('\r', '')
        (out/'results.txt').write_text(text)
        required = required_markers(args)
        manifest['passed'] = all(marker in text for marker in required)
        if manifest['passed']:
            manifest['error'] = None
            manifest['passed_from_persisted_log'] = True
    print(error)
finally:
    if guest:
        guest.close()
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
if not manifest['passed']:
    raise SystemExit(f'BSD_DESKTOP_NET_FAILED: {out/"serial.log"}')
print('BSD_DESKTOP_NET_PASS', out)
