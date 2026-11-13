#!/usr/bin/python3
# SPDX-License-Identifier: BSD-3-Clause
"""One-shot I2C pointer diagnostics. No keyboard events or coordinates logged."""
import ctypes
import fcntl
import glob
import os
import select
import struct
import time

def log(message):
    text = "[TOUCHPAD] " + message + "\n"
    print(text, end="", flush=True)
    try:
        fd = os.open("/dev/kmsg", os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(fd, text.encode())
        finally:
            os.close(fd)
    except OSError:
        pass

def query(fd, nr, size):
    data = bytearray(size)
    fcntl.ioctl(fd, (2 << 30) | (size << 16) | (ord("E") << 8) | nr, data)
    return data

def probe(path):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    try:
        bus, vendor, product, version = struct.unpack("HHHH", query(fd, 2, 8))
        if bus != 0x18:
            return False
        axes = int.from_bytes(query(fd, 0x23, 8), "little")  # EVIOCGBIT(EV_ABS)
        if not (axes & (1 << 0x35) and axes & (1 << 0x36)):
            return False
        log(f"node={path} native evdev id={vendor:04x}:{product:04x}")
        for axis in (0, 1, 0x2f, 0x35, 0x36, 0x39):
            info = struct.unpack("6i", query(fd, 0x40 + axis, 24))
            log(f"axis={axis:02x} range={info[1]}..{info[2]} resolution={info[5]}")
        values = bytearray(struct.pack("6i", 0x39, 0, 0, 0, 0, 0))
        fcntl.ioctl(fd, (2 << 30) | (len(values) << 16) | (ord("E") << 8) | 10, values)
        log("MT slot snapshot query PASS")
        ev = ctypes.CDLL("libevdev.so.2")
        ev.libevdev_new_from_fd.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
        ev.libevdev_free.argtypes = [ctypes.c_void_p]
        device = ctypes.c_void_p()
        result = ev.libevdev_new_from_fd(fd, ctypes.byref(device))
        log(f"libevdev initialization result={result}")
        if not result:
            ev.libevdev_free(device)
    finally:
        os.close(fd)
    li = ctypes.CDLL("libinput.so.10")
    opened = set()
    OPEN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
    CLOSE = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)
    def open_device(name, flags, userdata):
        try:
            descriptor = os.open(name, flags)
            opened.add(descriptor)
            return descriptor
        except OSError as error:
            return -error.errno
    def close_device(descriptor, userdata):
        if descriptor in opened:
            opened.remove(descriptor)
            os.close(descriptor)
    class Interface(ctypes.Structure):
        _fields_ = [("open", OPEN), ("close", CLOSE)]
    interface = Interface(OPEN(open_device), CLOSE(close_device))
    li.libinput_path_create_context.argtypes = [ctypes.POINTER(Interface), ctypes.c_void_p]
    li.libinput_path_create_context.restype = ctypes.c_void_p
    li.libinput_path_add_device.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    li.libinput_path_add_device.restype = ctypes.c_void_p
    li.libinput_device_has_capability.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    li.libinput_unref.argtypes = [ctypes.c_void_p]
    context = li.libinput_path_create_context(ctypes.byref(interface), None)
    if not context:
        log("libinput context creation failed")
        return True
    try:
        log("libinput device initialization starting")
        device = li.libinput_path_add_device(context, path.encode())
        pointer = bool(device and li.libinput_device_has_capability(device, 1))
        log(f"libinput accepted={bool(device)} pointer={pointer}")
        if pointer:
            li.libinput_get_fd.argtypes = [ctypes.c_void_p]
            li.libinput_dispatch.argtypes = [ctypes.c_void_p]
            li.libinput_get_event.argtypes = [ctypes.c_void_p]
            li.libinput_get_event.restype = ctypes.c_void_p
            li.libinput_event_get_type.argtypes = [ctypes.c_void_p]
            li.libinput_event_destroy.argtypes = [ctypes.c_void_p]
            li.libinput_event_get_pointer_event.argtypes = [ctypes.c_void_p]
            li.libinput_event_get_pointer_event.restype = ctypes.c_void_p
            li.libinput_event_pointer_get_button_state.argtypes = [ctypes.c_void_p]
            event_fd = li.libinput_get_fd(context)
            counts = {"motion": 0, "press": 0, "release": 0, "scroll": 0}
            until = time.monotonic() + 180
            report_at = time.monotonic() + 30
            while time.monotonic() < until:
                select.select([event_fd], [], [], 1)
                li.libinput_dispatch(context)
                while True:
                    event = li.libinput_get_event(context)
                    if not event:
                        break
                    kind = li.libinput_event_get_type(event)
                    if kind in (400, 401):
                        counts["motion"] += 1
                    elif kind == 402:
                        pe = li.libinput_event_get_pointer_event(event)
                        state = li.libinput_event_pointer_get_button_state(pe)
                        counts["press" if state else "release"] += 1
                    elif kind in (403, 404, 405, 406):
                        counts["scroll"] += 1
                    li.libinput_event_destroy(event)
                if time.monotonic() >= report_at:
                    log("live libinput " + " ".join(f"{key}={value}" for key, value in counts.items()))
                    report_at = time.monotonic() + 30
            log("live input diagnostic completed")
    finally:
        li.libinput_unref(context)
    return True

time.sleep(20)
log("automatic diagnostic starting")
for path in sorted(glob.glob("/dev/input/event*")):
    try:
        if probe(path):
            break
    except Exception as error:
        log(f"probe failed at {path}: {type(error).__name__}: {error}")
else:
    log("no matching I2C touchpad successfully probed")
