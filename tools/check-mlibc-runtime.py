#!/usr/bin/env python3
"""Run real Native libc boundary and application tests in an isolated RV64 or AArch64 VM.

Requires built programs, kernel and FAT image; never changes original images.
Every case must print its marker, return zero, and permit a natural shutdown.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import time
import socket
import http.server
import threading

ROOT = Path(__file__).resolve().parent.parent


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['riscv64', 'aarch64'], default='riscv64')
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--apps', type=Path, required=True)
    parser.add_argument('--shared-sysroot', type=Path)
    parser.add_argument('--locales', type=Path, help='locale directory containing C.utf8, en_US.utf8, de_DE.utf8')
    parser.add_argument('--case', action='append', choices=['semantics', 'hello', 'fork', 'mksh', 'sbase', 'threads', 'signals', 'dynamic', 'network', 'software', 'native'])
    parser.add_argument('--host-network', action='store_true', help='exercise real virtio/lwIP TCP against an isolated local HTTP server')
    parser.add_argument('--capture-network', action='store_true', help='retain QEMU Ethernet traffic as network.pcap for transport diagnosis')
    parser.add_argument('--software', action='store_true', help='also run built Native Lua and SQLite programs')
    parser.add_argument('--upstream', action='store_true', help='also run built mlibc-upstream-* programs')
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--cycles', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.case and 'software' in args.case: args.software = True
    output = args.output.resolve()
    if args.timeout <= 0 or args.cycles < 1 or (output.exists() and any(output.iterdir())):
        parser.error('positive timeout and empty output required; preserve failed evidence')
    output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, arch=args.arch, cpus=args.cpus, cases=[], inputs={},
                  kernel_rebuilt_by_this_runner=False)
    log = output / 'run.log'

    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def inject(source, destination):
        report['inputs'][str(source)] = sha(source)
        subprocess.run(['mcopy', '-o', '-i', str(output / 'fat32.img'),
                        str(source), '::/' + destination], check=True)

    process = None
    httpd = None
    refused = None
    save()
    try:
        shutil.copy2(args.kernel, output / 'kernel.elf')
        subprocess.run(['cp', '--sparse=always', str(args.image),
                        str(output / 'fat32.img')], check=True)
        report['inputs'][str(args.kernel)] = sha(args.kernel)
        report['inputs'][str(args.image)] = sha(args.image)
        for program in sorted(args.apps.iterdir()):
            if program.is_file() and program.name.startswith('mlibc-'):
                inject(program, program.name)
        for name in ['mksh', 'sbase']:
            inject(ROOT / ('user/tests/test_mlibc_' + name + '.sh'),
                   'test-mlibc-' + name + '.sh')
        if args.locales:
            def make_directory(name):
                found = subprocess.run(['mdir', '-i', str(output / 'fat32.img'), '::/' + name],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if found.returncode:
                    subprocess.run(['mmd', '-i', str(output / 'fat32.img'), '::/' + name], check=True)
            make_directory('mlibc-locales')
            for name in ['C.utf8', 'en_US.utf8', 'de_DE.utf8']:
                directory = args.locales / name
                if not directory.is_dir():
                    raise ValueError('required locale directory missing: ' + str(directory))
                make_directory('mlibc-locales/' + name)
                for item in sorted(directory.rglob('*')):
                    destination = 'mlibc-locales/' + name + '/' + str(item.relative_to(directory))
                    if item.is_dir():
                        make_directory(destination)
                    elif item.is_file():
                        inject(item, destination)
        tag = 'rv' if args.arch == 'riscv64' else 'aarch64'
        cases = [
            ('/bin/mlibc-semantics-rv', 'MLIBC_SEMANTICS: PASS'),
            ('/bin/mlibc-hello-rv', 'MLIBC_A20: PASS'),
            ('/bin/mlibc-fork-rv', 'MLIBC_FORK: PASS'),
            ('/bin/mlibc-mksh-rv /bin/test-mlibc-mksh.sh', 'MKSH_MLIBC: PASS'),
            ('/bin/sh /bin/test-mlibc-sbase.sh', 'MLIBC_SBASE: PASS'),
            ('/bin/mlibc-threads-rv', 'MLIBC_THREADS: PASS'),
            ('/bin/mlibc-signals-rv', 'MLIBC_SIGNALS: PASS'),
        ]
        if args.shared_sysroot:
            for name in ['libc.so', 'ld.so']:
                inject(args.shared_sysroot / 'lib' / name, 'lib/' + name)
            inject(args.apps / 'libmlibcplug.so', 'lib/libmlibcplug.so')
            cases.append(('unset LD_LIBRARY_PATH; /bin/mlibc-dynamic-rv', 'DYNAMIC: PASS'))
        if args.case:
            selected = set(args.case)
            labels = ['semantics', 'hello', 'fork', 'mksh', 'sbase', 'threads', 'signals', 'dynamic']
            if 'dynamic' in selected and not args.shared_sysroot:
                raise ValueError('dynamic case requires --shared-sysroot')
            cases = [case for label, case in zip(labels, cases) if label in selected]
        if not args.case or 'network' in args.case:
            cases.append(('/bin/mlibc-network-rv', 'MLIBC_NETWORK: PASS'))
        if args.case and 'native' in args.case:
            cases += [('/bin/native-handle-rv', 'xattr ok'),
                      ('/bin/native-contract-rv', 'NATIVE_CONTRACT: PASS'),
                      ('/bin/native-deepen-rv', 'NATIVE_DEEPEN: PASS'),
                      ('/bin/native-ipc-rv', 'NATIVE_IPC: PASS'),
                      ('/bin/abi_behavior', 'ABI_BEHAVIOR: PASS'),
                      ('/bin/native-abi-behavior-rv', 'ABI_BEHAVIOR: PASS')]
        if args.software:
            for name in ['lua-' + tag, 'sqlite-' + tag, 'sqlite-test-' + tag]:
                if not (args.apps / ('mlibc-' + name)).is_file():
                    raise ValueError('software program missing: ' + name)
            inject(ROOT / 'user/tests/test_mlibc_lua.lua', 'test-mlibc-lua.lua')
            cases += [
                ('/bin/mlibc-lua-rv /bin/test-mlibc-lua.lua', 'LUA_NATIVE: PASS'),
                ('/bin/mlibc-sqlite-test-rv', 'SQLITE_NATIVE: PASS'),
                ("/bin/mlibc-sqlite-rv :memory: \"WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<10) SELECT 'NATIVE_SQLITE_CLI|' || sum(x) FROM n;\"", 'NATIVE_SQLITE_CLI|55'),
            ]
        if args.upstream:
            prefix = 'LOCPATH=/bin/mlibc-locales ' if args.locales else ''
            cases += [(prefix + '/bin/' + p.name, '') for p in sorted(args.apps.glob('mlibc-upstream-*')) if p.is_file()]
        if args.host_network:
            class Handler(http.server.BaseHTTPRequestHandler):
                def do_GET(self):
                    body = bytes((i * 37 + 11) & 255 for i in range(8192))
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.send_header('Connection', 'close')
                    self.end_headers()
                    self.wfile.write(body)
                def log_message(self, *_): pass
            httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            threading.Thread(target=httpd.serve_forever, daemon=True).start()
            refused = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            refused.bind(('127.0.0.1', 0)) # reserve an explicitly non-listening port
            port, closed_port = httpd.server_port, refused.getsockname()[1]
            report['host_network'] = dict(http_port=port, refused_port=closed_port)
            cases = [(case + f' --host {port} {closed_port}' if marker == 'MLIBC_NETWORK: PASS' else case, marker) for case, marker in cases]
        cases = [(case.replace('-rv', '-' + tag), marker) for case, marker in cases]
        cases *= args.cycles
        flags = ['ARCH=' + args.arch, 'ABI=both', 'BRINGUP=0', f'NR_CPUS={args.cpus}']
        if args.cpus > 1:
            flags.append('ALLOW_UNVERIFIED_SMP=1')
        command = shlex.split(subprocess.check_output(
            ['make', '-s', *flags, '_qemu_argv'], cwd=ROOT, text=True).strip())
        command[command.index('-kernel') + 1] = str(output / 'kernel.elf')
        command[command.index('-drive') + 1] = f'file={output / "fat32.img"},if=none,format=raw,id=x0'
        command += ['-snapshot']
        if args.capture_network:
            command += ['-object', 'filter-dump,id=mlibc-capture,netdev=net,file=' + str(output / 'network.pcap')]
        if args.host_network:
            command += ['-append', 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.tcpmode=lwip']
        report['command'] = command
        report['driver_sha256'] = sha(Path(__file__))
        save()

        def wait_for(predicate):
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                content = log.read_text(errors='replace')
                if re.search(r'\[PANIC\]|Kernel panic|Kernel BUG|LOCK-STALL|ld.so panic|mlibc panic', content):
                    raise RuntimeError('kernel/libc panic; see run.log')
                value = predicate(content)
                if value:
                    return value
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited before completing the workload')
                time.sleep(0.05)
            raise RuntimeError('guest timeout; see run.log')

        with log.open('wb') as stream:
            process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE,
                                       stdout=stream, stderr=subprocess.STDOUT)
            wait_for(lambda text: re.search(r'(?:^|[\r\n])# ', text))
            for index, (case, marker) in enumerate(cases):
                start = len(log.read_text(errors='replace'))
                label = 'MLIBC_RUNTIME_STATUS_' + str(index) + ':'
                process.stdin.write((case + '; echo ' + label + '$?\n').encode())
                process.stdin.flush()
                status = wait_for(lambda text: re.search(
                    r'(?:^|[\r\n])' + label + r'(\d+)(?:[\r\n]|$)', text[start:]))
                # Consume the completed shell prompt before sending another command.
                wait_for(lambda text: re.search(r'(?:^|[\r\n])# ', text[start:][status.end():]))
                segment = log.read_text(errors='replace')[start:]
                passed = int(status[1]) == 0 and marker in segment and ': FAIL' not in segment
                if marker == 'MLIBC_THREADS: PASS':
                    online = re.search(r'start online=([0-9a-f]+)', segment)
                    passed &= bool(online) and int(online[1], 16) == (1 << args.cpus) - 1
                if marker == 'DYNAMIC: PASS':
                    passed &= 'DYNAMIC: reexec-ok' in segment and 'DYNAMIC: atexit' in segment
                result = dict(command=case, exit_status=int(status[1]), passed=passed)
                report['cases'].append(result)
                save()
                print(json.dumps(result), flush=True)
            process.stdin.write(b'poweroff\n')
            process.stdin.flush()
            wait_for(lambda text: 'System is going down for power-off NOW' in text)
            process.wait(timeout=15)
            report['passed'] = all(case['passed'] for case in report['cases']) and process.returncode == 0
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        if httpd: httpd.shutdown(); httpd.server_close()
        if refused: refused.close()
        if process:
            report['host_terminated'] = process.poll() is None
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                report['passed'] = False
            report['returncode'] = process.returncode
            process.stdin.close()
        save()
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
