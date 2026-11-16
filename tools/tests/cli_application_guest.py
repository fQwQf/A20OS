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


def shells():
    command = '''set -e
x=$(printf 'one\\ntwo\\n' | cat)
test "$x" = "$(printf 'one\\ntwo')"
sh -c 'exit 7' & p=$!
if wait "$p"; then exit 9; else test $? = 7; fi
printf 'CLI_SHELL_OK\\n'
'''
    for shell in ('/bin/bash', '/bin/zsh'):
        assert run([shell, '-fc', command]) == 'CLI_SHELL_OK\n', shell


def mount_tmpfs():
    with tempfile.TemporaryDirectory(prefix='a20-cli-mount-') as directory:
        run(['mount', '-t', 'tmpfs', '-o', 'size=4m', 'tmpfs', directory])
        try:
            lines = Path('/proc/self/mountinfo').read_text().splitlines()
            assert any(row.split()[4] == directory and ' - tmpfs ' in row for row in lines), '\n'.join(lines)
            path = Path(directory) / 'payload'
            path.write_text('CLI_MOUNT_OK')
            assert path.read_text() == 'CLI_MOUNT_OK'
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
    args = parser.parse_args()
    results = {}
    for name, operation in (
            ('shells', shells), ('mount', mount_tmpfs), ('interfaces', interfaces),
            ('http', lambda: http(args.http_url)), ('udp-dns', lambda: dns(args.dns_host, args.dns_port))):
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
