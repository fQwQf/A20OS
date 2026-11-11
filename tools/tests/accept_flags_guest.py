#!/usr/bin/python3
"""Linux oracle and guest regression: accept4 flags and delayed request I/O."""
import ctypes
import errno
import fcntl
import os
import select
import socket
import tempfile
import threading
import time

libc = ctypes.CDLL(None, use_errno=True)
libc.accept4.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
libc.accept4.restype = ctypes.c_int
cases = 0
with tempfile.TemporaryDirectory(prefix='accept-flags-') as directory:
    for domain, kind in ((socket.AF_UNIX, socket.SOCK_STREAM),
                         (socket.AF_UNIX, socket.SOCK_SEQPACKET),
                         (socket.AF_INET, socket.SOCK_STREAM)):
        for nonblock in (False, True):
            for flags in (0, socket.SOCK_CLOEXEC, socket.SOCK_NONBLOCK,
                          socket.SOCK_NONBLOCK | socket.SOCK_CLOEXEC):
                print(f'ACCEPT_FLAGS: case={cases} family={domain} kind={kind} listener_nonblock={nonblock} flags={flags}', flush=True)
                with socket.socket(domain, kind) as listener:
                    address = directory + '/' + str(cases) if domain == socket.AF_UNIX else ('127.0.0.1', 0)
                    listener.bind(address)
                    listener.listen(1)
                    listener.setblocking(not nonblock)
                    if nonblock:
                        assert libc.accept4(listener.fileno(), None, None, flags) == -1
                        assert ctypes.get_errno() in (errno.EAGAIN, errno.EWOULDBLOCK)
                    connected, send = threading.Event(), threading.Event()
                    errors = []
                    def client():
                        try:
                            with socket.socket(domain, kind) as peer:
                                peer.connect(listener.getsockname())
                                connected.set()
                                assert send.wait(5)
                                time.sleep(0.05)
                                peer.sendall(b'status')
                        except BaseException as error:
                            errors.append(error)
                            connected.set()
                    thread = threading.Thread(target=client)
                    thread.start()
                    accepted = -1
                    try:
                        assert connected.wait(5) and not errors, errors
                        assert select.select([listener], [], [], 5)[0], 'listener not ready after connect'
                        accepted = libc.accept4(listener.fileno(), None, None, flags)
                        assert accepted >= 0, ctypes.get_errno()
                        assert bool(fcntl.fcntl(accepted, fcntl.F_GETFL) & os.O_NONBLOCK) == bool(flags & socket.SOCK_NONBLOCK)
                        assert bool(fcntl.fcntl(accepted, fcntl.F_GETFD) & fcntl.FD_CLOEXEC) == bool(flags & socket.SOCK_CLOEXEC)
                        if flags & socket.SOCK_NONBLOCK:
                            try:
                                os.read(accepted, 6)
                                raise AssertionError('nonblocking request read unexpectedly succeeded')
                            except BlockingIOError:
                                pass
                            fcntl.fcntl(accepted, fcntl.F_SETFL, fcntl.fcntl(accepted, fcntl.F_GETFL) & ~os.O_NONBLOCK)
                        send.set()
                        # The delayed client deliberately has not written yet.
                        # A mistakenly inherited NONBLOCK returns EAGAIN here.
                        assert os.read(accepted, 6) == b'status'
                        cases += 1
                    finally:
                        send.set()
                        if accepted >= 0:
                            os.close(accepted)
                        thread.join(timeout=6)
                    assert not thread.is_alive() and not errors, errors
print('ACCEPT_FLAGS: PASS delayed request and flags cases=' + str(cases), flush=True)
