#!/usr/bin/env python3
"""Run the CLI world's shells, mount and network tools against local fixtures."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import traceback


def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise RuntimeError(f'{argv!r} exited {result.returncode}: '
                           + (result.stdout + result.stderr)[-4000:])
    return result.stdout


def console():
    import array
    import fcntl
    import os
    import struct
    import termios
    import time

    fd = os.open('/dev/tty', os.O_RDWR | os.O_NOCTTY)
    try:
        assert os.isatty(0) and os.isatty(fd)
        sid = array.array('i', [0])
        fcntl.ioctl(fd, getattr(termios, 'TIOCGSID', 0x5429), sid, True)
        assert sid[0] == os.getsid(0) and sid[0] > 1, (sid[0], os.getsid(0))
        assert os.tcgetpgrp(fd) == os.getpgrp()
        rows, columns, _, _ = struct.unpack('HHHH', fcntl.ioctl(fd, termios.TIOCGWINSZ, b'\0' * 8))
        assert (rows, columns) == (40, 120), (rows, columns)
        assert (os.environ.get('LINES'), os.environ.get('COLUMNS')) == ('40', '120')
        assert str(Path('/proc/1/exe').readlink()).endswith('/busybox')
        saved = termios.tcgetattr(fd)
        assert run(['/usr/lib/a20/console-size']) == 'COLUMNS=120; LINES=40\n'
        assert termios.tcgetattr(fd) == saved, 'size query did not restore terminal mode'
        resized = run(['/bin/zsh', '-fc',
                       'source /root/.zshrc\nCOLUMNS=80\nLINES=24\nresize-console\n'
                       'test "$COLUMNS,$LINES" = "120,40"\n'])
        assert resized == 'console is now 120x40\n', resized


        # PID 1 must reap a descendant orphaned after the shell's child exits.
        read_fd, write_fd = os.pipe()
        child = os.fork()
        if child == 0:
            os.close(read_fd)
            grandchild = os.fork()
            if grandchild == 0:
                os.write(write_fd, str(os.getpid()).encode())
                os.close(write_fd)
                time.sleep(.1)
            os._exit(0)
        os.close(write_fd)
        assert os.waitpid(child, 0) == (child, 0)
        orphan = int(os.read(read_fd, 32))
        os.close(read_fd)
        deadline = time.monotonic() + 3
        while Path(f'/proc/{orphan}/stat').exists() and time.monotonic() < deadline:
            time.sleep(.05)
        assert not Path(f'/proc/{orphan}/stat').exists(), 'PID 1 left an orphan zombie'
        print('CLI_CONSOLE:', json.dumps({'sid': sid[0], 'pgrp': os.getpgrp(),
                                         'rows': rows, 'columns': columns,
                                         'orphan_reaped': orphan}), flush=True)
    finally:
        os.close(fd)


def shells():
    import errno
    import os
    import resource

    # A20's default is unlimited; reporting its old internal zero sentinel
    # makes bash size its child-status table to zero during command substitution.
    assert resource.getrlimit(resource.RLIMIT_NPROC) == (resource.RLIM_INFINITY,) * 2
    child = os.fork()
    if child == 0:
        try:
            os.setuid(65534)
            resource.setrlimit(resource.RLIMIT_NPROC, (0, 0))
            assert resource.getrlimit(resource.RLIMIT_NPROC) == (0, 0)
            try:
                unexpected = os.fork()
            except OSError as error:
                assert error.errno == errno.EAGAIN, error
            else:
                if unexpected == 0:
                    os._exit(99)
                os.waitpid(unexpected, 0)
                raise AssertionError('zero NPROC permitted an unprivileged fork')
        except Exception:
            traceback.print_exc()
            os._exit(1)
        os._exit(0)
    assert os.waitpid(child, 0) == (child, 0)
    command = '''set -e
x=$(printf 'one\\ntwo\\n' | cat)
test "$x" = "$(printf 'one\\ntwo')"
sh -c 'exit 7' & p=$!
if wait "$p"; then exit 9; else test $? = 7; fi
printf 'CLI_SHELL_OK\\n'
'''
    for shell in ('/bin/bash', '/bin/zsh'):
        assert run([shell, '-fc', command]) == 'CLI_SHELL_OK\n', shell
        print('CLI_SHELL:', shell, 'PASS', flush=True)


def mount_tmpfs():
    import ctypes
    import os
    import re
    import select

    def fields(path, column):
        return [(re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), row.split()[column]), row)
                for row in Path(path).read_text().splitlines()]

    def check_mount(path, expected, column):
        entries = fields(path, column)
        assert any(name == expected and 'tmpfs' in row.split()
                   for name, row in entries), (path, expected, entries)
        assert not any(name == '/extra' or name.startswith('/extra/')
                       for name, row in entries), (path, entries)
        return entries

    libc = ctypes.CDLL(None, use_errno=True)
    libc.unshare.argtypes = [ctypes.c_int]
    libc.umount2.argtypes = [ctypes.c_char_p, ctypes.c_int]
    with tempfile.TemporaryDirectory(prefix='a20-cli mount-') as directory:
        run(['mount', '-t', 'tmpfs', '-o', 'size=4m', 'tmpfs', directory])
        try:
            check_mount('/proc/self/mountinfo', directory, 4)
            check_mount('/proc/self/mounts', directory, 1)
            check_mount('/proc/mounts', directory, 1)
            path = Path(directory) / 'payload'
            path.write_text('CLI_MOUNT_OK')
            assert path.read_text() == 'CLI_MOUNT_OK'
            private = Path(directory) / 'private-mount'
            private.mkdir()
            ready_r, ready_w = os.pipe()
            stop_r, stop_w = os.pipe()
            child = os.fork()
            if child == 0:
                os.close(ready_r)
                os.close(stop_w)
                try:
                    assert libc.unshare(0x20000) == 0, ctypes.get_errno()
                    run(['mount', '-t', 'tmpfs', 'tmpfs', str(private)])
                    os.chroot(directory)
                    os.chdir('/')
                    os.write(ready_w, b'READY')
                    os.read(stop_r, 1)
                    assert libc.umount2(b'/private-mount', 0) == 0, ctypes.get_errno()
                except Exception:
                    traceback.print_exc()
                    os._exit(1)
                os._exit(0)
            os.close(ready_w)
            os.close(stop_r)
            try:
                assert select.select([ready_r], [], [], 10)[0], 'private mount child timed out'
                assert os.read(ready_r, 32) == b'READY', 'private mount child failed'
                # Reading another pid must use its namespace and its chroot.
                child_info = check_mount(f'/proc/{child}/mountinfo', '/private-mount', 4)
                child_mounts = check_mount(f'/proc/{child}/mounts', '/private-mount', 1)
                assert {name for name, row in child_info} == {'/', '/private-mount'}, child_info
                assert {name for name, row in child_mounts} == {'/', '/private-mount'}, child_mounts
                assert str(private) not in {name for name, row in fields('/proc/self/mountinfo', 4)}
                os.write(stop_w, b'X')
            except BaseException:
                os.kill(child, 9)
                raise
            finally:
                os.close(ready_r)
                os.close(stop_w)
                assert os.waitpid(child, 0) == (child, 0), 'private mount child did not cleanly exit'
        finally:
            run(['umount', directory])
        assert not (Path(directory) / 'payload').exists()


def interfaces():
    import errno
    import socket

    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 0) as probe:
        for option in (socket.SO_SNDBUF, socket.SO_RCVBUF):
            probe.setsockopt(socket.SOL_SOCKET, option, 1024 * 1024)
            assert 0 < probe.getsockopt(socket.SOL_SOCKET, option) <= 1024 * 1024
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 32)
        try:
            probe.sendto(b"x" * 33, (0, 0))
        except OSError as error:
            assert error.errno == errno.EMSGSIZE, error
        else:
            raise AssertionError("netlink send exceeded its configured datagram limit")
    links = json.loads(run(['ip', '-j', 'link', 'show']))
    addresses = json.loads(run(['ip', '-j', 'address', 'show']))
    assert links
    assert any(address.get('local') == '10.0.2.15'
               for device in addresses for address in device.get('addr_info', [])), addresses
    assert isinstance(json.loads(run(['ip', '-j', 'route', 'show'])), list)


def http(url):
    assert run(['curl', '--noproxy', '*', '--max-time', '10', '-fsS', url]) == 'A20_CLI_HTTP_OK\n'


def udp_errors():
    import ctypes
    import errno
    import select
    import socket
    import struct

    errqueue = getattr(socket, 'MSG_ERRQUEUE', 0x2000)
    for family, loop, level, option, origin, kind, code in (
            (socket.AF_INET, '127.0.0.1', socket.IPPROTO_IP, 11, 2, 3, 3),
            (socket.AF_INET6, '::1', socket.IPPROTO_IPV6, 25, 3, 1, 4)):
        with socket.socket(family, socket.SOCK_DGRAM) as reserve:
            reserve.bind((loop, 0))
            port = reserve.getsockname()[1]
        with socket.socket(family, socket.SOCK_DGRAM) as sock:
            sock.bind((loop, 0))
            sock.setsockopt(level, option, 1)
            assert sock.getsockopt(level, option) == 1
            poll = select.poll()
            poll.register(sock, 0)

            def send():
                payload = b'A20_ICMP_ERROR'
                assert sock.sendto(payload, (loop, port)) == len(payload)
                events = poll.poll(2000)
                assert any(event & select.POLLERR for fd, event in events), events
                assert not any(event & select.POLLHUP for fd, event in events), events
                return payload

            payload = send()
            assert sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR) == errno.ECONNREFUSED
            assert poll.poll(0), 'SO_ERROR must not consume the extended error queue'
            data, ancillary, flags, address = sock.recvmsg(128, 128, errqueue)
            assert data == payload and address[:2] == (loop, port), (data, address)
            assert flags & errqueue
            records = [c for c in ancillary if c[:2] == (level, option)]
            assert len(records) == 1, ancillary
            extended = struct.unpack_from('=IBBBBII', records[0][2])
            assert extended == (errno.ECONNREFUSED, origin, kind, code, 0, 0, 0), extended
            assert struct.unpack_from('=H', records[0][2], 16)[0] == family
            assert not poll.poll(0), 'consumed error remained ready'
            try:
                sock.recvmsg(1, 128, errqueue)
            except OSError as error:
                assert error.errno == errno.EAGAIN, error
            else:
                raise AssertionError('empty error queue did not return EAGAIN')

            payload = send()
            try:
                sock.recv(128, socket.MSG_DONTWAIT)
            except OSError as error:
                assert error.errno == errno.ECONNREFUSED, error
            else:
                raise AssertionError('ordinary receive missed the pending UDP error')
            data, ancillary, flags, address = sock.recvmsg(1, 1, errqueue)
            assert data == payload[:1] and not ancillary
            assert flags & socket.MSG_TRUNC and flags & socket.MSG_CTRUNC and flags & errqueue
            assert not poll.poll(0)

            if family == socket.AF_INET:
                send()
                class Header(ctypes.Structure):
                    _fields_ = [('name', ctypes.c_void_p), ('namelen', ctypes.c_uint),
                                ('iov', ctypes.c_void_p), ('iovlen', ctypes.c_size_t),
                                ('control', ctypes.c_void_p), ('controllen', ctypes.c_size_t),
                                ('flags', ctypes.c_int)]
                control = ctypes.create_string_buffer(128)
                name = ctypes.create_string_buffer(1)
                header = Header(ctypes.addressof(name), 0, None, 0,
                                ctypes.addressof(control), len(control), 0)
                libc = ctypes.CDLL(None, use_errno=True)
                libc.recvmsg.restype = ctypes.c_ssize_t
                assert libc.recvmsg(sock.fileno(), ctypes.byref(header), errqueue) == 0, ctypes.get_errno()
                assert header.namelen == 16 and name.raw == b'\x00'
                assert header.flags & errqueue and header.flags & socket.MSG_TRUNC
                assert not poll.poll(0)
                send()
                sock.setsockopt(level, option, 0)
                assert sock.getsockopt(level, option) == 0
                sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                assert not poll.poll(0), 'disabling did not purge queued errors'
            print('CLI_UDP_ERRORS:', family, 'PASS', flush=True)


def dns(host, port):
    result = run(['dig', '@' + host, '-p', str(port), 'example.test', 'A',
                  '+short', '+time=2', '+tries=1'])
    assert '192.0.2.123' in result.splitlines(), result
    result = run(['nslookup', '-port=' + str(port), 'example.test', host])
    assert '192.0.2.123' in result, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--http-url', required=True)
    parser.add_argument('--dns-host', default='10.0.2.2')
    parser.add_argument('--dns-port', type=int, required=True)
    parser.add_argument('--check', action='append', choices=(
        'console', 'shells', 'mount', 'interfaces', 'http', 'udp-errors', 'udp-dns'),
        help='Run only the named checks (repeatable); defaults to all checks.')
    args = parser.parse_args()
    results = {}
    for name, operation in (
            ('console', console), ('shells', shells), ('mount', mount_tmpfs), ('interfaces', interfaces),
            ('http', lambda: http(args.http_url)), ('udp-errors', udp_errors), ('udp-dns', lambda: dns(args.dns_host, args.dns_port))):
        if args.check and name not in args.check:
            continue
        try:
            operation()
            results[name] = {'status': 'PASS'}
        except Exception as error:
            results[name] = {'status': 'FAIL', 'error': repr(error),
                             'traceback': traceback.format_exc()}
        print('CLI_APPLICATION:', name, results[name]['status'], flush=True)
    print(json.dumps(results, indent=2), flush=True)
    passed = all(item['status'] == 'PASS' for item in results.values())
    print('CLI_APPLICATION: ' + ('PASS' if passed else 'FAIL'), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
