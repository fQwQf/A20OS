#!/usr/bin/env python3
"""Guest-only evdev/sysfs/udev/libevdev regression; not installed in images.

Run in the started XFCE session with Python 3 and libevdev available. Real
pointer movement, menu clicks, and window dragging remain manual GUI checks.
"""
import ctypes
import fcntl
import glob
import os
from pathlib import Path
import subprocess


def query(fd, nr, size=128):
    buf = bytearray(size)
    fcntl.ioctl(fd, (2 << 30) | (size << 16) | (ord('E') << 8) | nr, buf)
    return bytes(buf)


def bitmap(path):
    value = 0
    for word in Path(path).read_text().split():
        value = (value << (8 * ctypes.sizeof(ctypes.c_ulong))) | int(word, 16)
    return value


def main():
    lib = ctypes.CDLL('libevdev.so.2')
    lib.libevdev_new_from_fd.argtypes = [ctypes.c_int,
                                       ctypes.POINTER(ctypes.c_void_p)]
    lib.libevdev_free.argtypes = [ctypes.c_void_p]
    keyboard = mouse = False
    devices = glob.glob('/dev/input/event*')
    assert devices, 'no physical evdev nodes'
    for path in sorted(devices):
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        try:
            name = query(fd, 6).split(b'\0', 1)[0].decode()
            keys = int.from_bytes(query(fd, 0x21), 'little')
            st = os.fstat(fd)
            aliases = ['/sys/class/input/' + Path(path).name,
                       '/sys/devices/virtual/input/' + Path(path).name,
                       '/sys/dev/char/%d:%d' % (os.major(st.st_rdev),
                                               os.minor(st.st_rdev))]
            for alias in aliases:
                assert Path(alias + '/name').read_text().strip() == name, alias
                assert bitmap(alias + '/capabilities/key') == keys, alias
            props = subprocess.check_output(
                ['udevadm', 'info', '--query=property', '--name=' + path],
                text=True).splitlines()
            if keys & (1 << 30):  # KEY_A
                assert 'ID_INPUT_KEYBOARD=1' in props, path
                keyboard = True
            if keys & (1 << 272):  # BTN_LEFT
                assert 'ID_INPUT_MOUSE=1' in props, path
                mouse = True
            dev = ctypes.c_void_p()
            result = lib.libevdev_new_from_fd(fd, ctypes.byref(dev))
            assert result == 0, (path, 'libevdev initialization', result)
            lib.libevdev_free(dev)
            print('XFCE_INPUT:', path, name, 'PASS', flush=True)
        finally:
            os.close(fd)
    assert keyboard and mouse, 'need both keyboard and mouse'
    print('XFCE_INPUT: sysfs+udev+libevdev PASS', flush=True)


if __name__ == '__main__':
    main()
