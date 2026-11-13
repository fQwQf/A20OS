#!/usr/bin/env python3
"""Linux ABI FIFO conformance on real guest filesystems (also host oracle).

Covers inode metadata, peer rendezvous, disconnected/nonblocking endpoints,
PIPE_BUF atomicity, poll/epoll, signals, aliases and last-close lifetime.
"""
import array
import ctypes
import errno
import fcntl
import mmap
import os
import select
import signal
import stat
import tempfile
import time
import traceback


def raises_errno(code, call):
    try:
        call()
    except OSError as error:
        assert error.errno == code, (code, error)
    else:
        raise AssertionError('expected errno %d' % code)


def wait_child(pid):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        got, status = os.waitpid(pid, os.WNOHANG)
        if got:
            assert os.waitstatus_to_exitcode(status) == 0, status
            return
        time.sleep(.01)
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    raise AssertionError('FIFO child timed out')


def check(base):
    with tempfile.TemporaryDirectory(prefix='fifo-qa-', dir=base) as directory:
        path = directory + '/queue'
        raises_errno(errno.ENOENT, lambda: os.mkfifo(''))
        previous = os.umask(0o027)
        try:
            os.mkfifo(path, 0o666)
        finally:
            os.umask(previous)
        metadata = os.stat(path)
        assert stat.S_ISFIFO(metadata.st_mode) and stat.S_IMODE(metadata.st_mode) == 0o640
        raises_errno(errno.EEXIST, lambda: os.mkfifo(path))
        assert os.stat(path).st_ino == metadata.st_ino
        if hasattr(os, 'O_PATH'):
            handle = os.open(path, os.O_PATH)
            assert stat.S_ISFIFO(os.fstat(handle).st_mode)
            raises_errno(errno.ENXIO, lambda: os.open(path, os.O_WRONLY | os.O_NONBLOCK))
            os.close(handle)
        raises_errno(errno.ENXIO, lambda: os.open(path, os.O_WRONLY | os.O_NONBLOCK))
        r = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        assert os.read(r, 1) == b''
        poll = select.poll()
        poll.register(r, select.POLLIN)
        assert poll.poll(0) == [], 'no writer has connected: no POLLHUP yet'
        w = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        raises_errno(errno.EAGAIN, lambda: os.read(r, 1))
        os.write(w, b'one')
        assert poll.poll(1000)[0][1] & select.POLLIN
        amount = array.array('i', [0])
        fcntl.ioctl(r, 0x541B, amount, True)  # FIONREAD
        assert amount[0] == 3
        assert os.read(r, 1) == b'o'
        os.close(w)
        flags = poll.poll(1000)[0][1]
        assert flags & select.POLLHUP and flags & select.POLLIN, flags
        assert os.read(r, 8) == b'ne' and os.read(r, 1) == b''
        w = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        assert poll.poll(0) == [], 'reconnected writer clears HUP'
        os.close(r)
        wpoll = select.poll()
        wpoll.register(w, select.POLLOUT)
        assert wpoll.poll(0)[0][1] & select.POLLERR
        raises_errno(errno.EPIPE, lambda: os.write(w, b'x'))
        os.close(w)
        # Empty contents after every description closes; RDWR needs no peer.
        both = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        raises_errno(errno.EAGAIN, lambda: os.read(both, 1))
        raises_errno(errno.EINVAL, lambda: os.fsync(both))
        raises_errno(errno.ENODEV, lambda: mmap.mmap(both, 4096))
        epoll = select.epoll()
        epoll.register(both, select.EPOLLIN | select.EPOLLOUT)
        assert epoll.poll(0)[0][1] & select.EPOLLOUT
        block = b'a' * 4096
        capacity = fcntl.fcntl(both, 1032)  # F_GETPIPE_SZ
        assert capacity >= 4096 and capacity % 4096 == 0
        for _ in range(capacity // 4096):
            assert os.write(both, block) == 4096
        raises_errno(errno.EAGAIN, lambda: os.write(both, b'b' * 4096))
        assert os.read(both, 1) == b'a'
        raises_errno(errno.EAGAIN, lambda: os.write(both, b'b' * 4096))
        assert os.read(both, 4095) == b'a' * 4095
        assert os.write(both, b'b' * 4096) == 4096
        assert epoll.poll(0)[0][1] & select.EPOLLIN
        epoll.close()
        os.close(both)
        # Pending blocking read opens count as peers; immediate writer close
        # must still satisfy the rendezvous even if the opener runs later.
        for _ in range(12):
            child = os.fork()
            if child == 0:
                try:
                    fd = os.open(path, os.O_RDONLY)
                    assert os.read(fd, 8) == b'hello'
                    assert os.read(fd, 1) == b''
                    os.close(fd)
                    os._exit(0)
                except BaseException:
                    traceback.print_exc()
                    os._exit(1)
            fd = os.open(path, os.O_WRONLY)
            os.write(fd, b'hello')
            os.close(fd)
            wait_child(child)
        # Reverse order: a pending writer must wake when a reader arrives.
        child = os.fork()
        if child == 0:
            try:
                fd = os.open(path, os.O_WRONLY)
                os.write(fd, b'reverse')
                os.close(fd)
                os._exit(0)
            except BaseException:
                traceback.print_exc()
                os._exit(1)
        fd = os.open(path, os.O_RDONLY)
        assert os.read(fd, 16) == b'reverse'
        os.close(fd)
        wait_child(child)
        # Hard links share one stream. Unlink and path reuse remain distinct.
        alias = directory + '/alias'
        os.link(path, alias)
        r = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        w = os.open(alias, os.O_WRONLY | os.O_NONBLOCK)
        os.unlink(path)
        os.mkfifo(path)
        isolated = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        os.write(w, b'alias')
        raises_errno(errno.EAGAIN, lambda: os.read(isolated, 1))
        assert os.read(r, 8) == b'alias'
        for fd in (r, w, isolated):
            os.close(fd)
        os.unlink(alias)
        if os.uname().machine == 'x86_64':
            # Restrict only FIFO creation. A symlink under the allowed path
            # must not grant creation in the disallowed directory it targets.
            allowed = directory + '/allowed'
            denied = directory + '/denied'
            os.mkdir(allowed);os.mkdir(denied)
            os.symlink(denied, allowed + '/escape')
            child = os.fork()
            if child == 0:
                try:
                    class Rule(ctypes.Structure):
                        _pack_ = 1
                        _fields_ = [('access', ctypes.c_uint64), ('fd', ctypes.c_int32)]
                    libc = ctypes.CDLL(None, use_errno=True)
                    handled = ctypes.c_uint64(1 << 10)
                    ruleset = libc.syscall(444, ctypes.byref(handled), 8, 0)
                    assert ruleset >= 0, ctypes.get_errno()
                    directory_fd = os.open(allowed, os.O_PATH | os.O_DIRECTORY)
                    rule = Rule(1 << 10, directory_fd)
                    assert libc.syscall(445, ruleset, 1, ctypes.byref(rule), 0) == 0, ctypes.get_errno()
                    assert libc.prctl(38, 1, 0, 0, 0) == 0
                    assert libc.syscall(446, ruleset, 0) == 0, ctypes.get_errno()
                    os.mkfifo(allowed + '/ok')
                    regular = os.open(allowed + '/regular', os.O_CREAT | os.O_RDWR, stat.S_IFIFO | 0o600)
                    assert stat.S_ISREG(os.fstat(regular).st_mode)
                    os.close(regular)
                    raises_errno(errno.EACCES, lambda: os.mkfifo(denied + '/direct'))
                    raises_errno(errno.EACCES, lambda: os.mkfifo(allowed + '/escape/no'))
                    print('FIFO_LANDLOCK: PASS allow, direct deny, symlink deny, regular open type', flush=True)
                    os._exit(0)
                except BaseException:
                    traceback.print_exc()
                    os._exit(1)
            wait_child(child)
        # Signal interruption must undo endpoint counts and pending opens.
        child = os.fork()
        if child == 0:
            def alarm(*_):
                raise TimeoutError()
            signal.signal(signal.SIGALRM, alarm)
            signal.alarm(1)
            try:
                os.open(path, os.O_RDONLY)
            except TimeoutError:
                os._exit(0)
            os._exit(1)
        wait_child(child)
        raises_errno(errno.ENXIO, lambda: os.open(path, os.O_WRONLY | os.O_NONBLOCK))
        # Several separate descriptions must preserve short-write records.
        both = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        children = []
        for byte in (b'A', b'B', b'C', b'D'):
            child = os.fork()
            if child == 0:
                try:
                    os.close(both)
                    fd = os.open(path, os.O_WRONLY)
                    for _ in range(80):
                        assert os.write(fd, byte * 128) == 128
                    os.close(fd)
                    os._exit(0)
                except BaseException:
                    traceback.print_exc()
                    os._exit(1)
            children.append(child)
        payload = b''
        deadline = time.monotonic() + 10
        while len(payload) < 4 * 80 * 128 and time.monotonic() < deadline:
            try:
                payload += os.read(both, 8192)
            except BlockingIOError:
                time.sleep(.001)
        assert len(payload) == 4 * 80 * 128, len(payload)
        assert all(len(set(payload[i:i+128])) == 1 for i in range(0, len(payload), 128))
        for child in children:
            wait_child(child)
        os.close(both)
        print('FIFO: PASS filesystem=' + base, flush=True)


def main():
    signal.alarm(90)
    for base in ('/tmp', '/dev/shm'):
        check(base)
    # Explicitly exercise both native x86_64 entry points, independently of
    # which libc implementation backs Python's os.mkfifo.
    if os.uname().machine == 'x86_64':
        libc = ctypes.CDLL(None, use_errno=True)
        for number, at in ((133, False), (259, True)):
            path = ('/tmp/fifo-syscall-%d-%d' % (os.getpid(), number)).encode()
            args = (ctypes.c_int(-100), ctypes.c_char_p(path), ctypes.c_uint(stat.S_IFIFO | 0o600), ctypes.c_uint(0)) if at else (ctypes.c_char_p(path), ctypes.c_uint(stat.S_IFIFO | 0o600), ctypes.c_uint(0))
            assert libc.syscall(ctypes.c_long(number), *args) == 0, ctypes.get_errno()
            assert stat.S_ISFIFO(os.stat(path).st_mode)
            os.unlink(path)
        print('FIFO_ABI: PASS legacy mknod and mknodat', flush=True)
    signal.alarm(0)
    print('FIFO: ALL PASS', flush=True)


if __name__ == '__main__':
    main()
