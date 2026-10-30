#!/usr/bin/env python3
"""Guest-only IPv4/IPv6 TCP listen/accept loopback gate."""
import socket
import sys


def one(family, address):
    listener = socket.socket(family, socket.SOCK_STREAM)
    listener.settimeout(5)
    listener.bind((address, 0))
    listener.listen(8)
    port = listener.getsockname()[1]
    client = socket.create_connection((address, port), timeout=5)
    accepted, peer = listener.accept()
    accepted.settimeout(5)
    payload = b'bsd-listen-accept-' + str(family).encode()
    client.sendall(payload)
    got = b''
    while len(got) < len(payload):
        chunk = accepted.recv(len(payload) - len(got))
        if not chunk:
            raise RuntimeError('short read')
        got += chunk
    if got != payload:
        raise RuntimeError('payload mismatch')
    accepted.sendall(got)
    echoed = b''
    while len(echoed) < len(payload):
        chunk = client.recv(len(payload) - len(echoed))
        if not chunk:
            raise RuntimeError('short echo')
        echoed += chunk
    accepted.close()
    client.close()
    listener.close()
    print(f'BSD_SOCKET_LOOPBACK_PASS family={family} port={port}')


def refused(family, address):
    for attempt in range(6):
        probe = socket.socket(family, socket.SOCK_STREAM)
        probe.settimeout(3)
        try:
            probe.connect((address, 1))
            print(f'BSD_SOCKET_REFUSED_UNEXPECTED family={family} attempt={attempt}',
                  flush=True)
        except OSError as error:
            print(f'BSD_SOCKET_REFUSED family={family} attempt={attempt} '
                  f'error={error.errno}', flush=True)
        finally:
            probe.close()


try:
    refused(socket.AF_INET, '127.0.0.1')
    one(socket.AF_INET, '127.0.0.1')
    one(socket.AF_INET6, '::1')
except Exception as error:
    print(f'BSD_SOCKET_LOOPBACK_FAIL {error!r}')
    sys.exit(1)
