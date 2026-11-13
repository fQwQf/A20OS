#!/usr/bin/python3
"""Disposable real IBus/Rime GUI probe, served to a snapshot guest only."""
import os
import atexit
import fcntl
import signal
import select
import socket
import stat
from pathlib import Path
import subprocess
import time
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

for wakeup_kind in ('socket', 'pipe'):
    pair = socket.socketpair() if wakeup_kind == 'socket' else None
    if pair:
        for item in pair:
            item.setblocking(False)
        read_fd, write_fd = (item.fileno() for item in pair)
    else:
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
    previous_handler = signal.signal(signal.SIGUSR1, lambda *_: None)
    try:
        metadata = os.fstat(write_fd)
        assert stat.S_ISSOCK(metadata.st_mode) if pair else stat.S_ISFIFO(metadata.st_mode)
        duplicate = os.dup(write_fd)
        try:
            assert os.fstat(duplicate).st_ino == metadata.st_ino
        finally:
            os.close(duplicate)
        assert fcntl.fcntl(write_fd, fcntl.F_GETFL) & os.O_NONBLOCK
        old_wakeup = signal.set_wakeup_fd(write_fd)
        try:
            os.kill(os.getpid(), signal.SIGUSR1)
            assert select.select([read_fd], [], [], 5)[0], 'signal did not wake the fd'
            assert os.read(read_fd, 1) == bytes([signal.SIGUSR1])
        finally:
            signal.set_wakeup_fd(old_wakeup)
        print('WAKEUP_FD: PASS ' + wakeup_kind, flush=True)
    finally:
        signal.signal(signal.SIGUSR1, previous_handler)
        if pair:
            for item in pair:
                item.close()
        else:
            os.close(read_fd)
            os.close(write_fd)
loop = GLib.MainLoop()
GLib.timeout_add(100, lambda: (loop.quit(), False)[1])
loop.run()
print('WAKEUP_FD: PASS native PyGObject main loop', flush=True)

def run(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=60)
    print('INPUT_METHOD:', args[0], 'exit', result.returncode,
          result.stdout.strip(), result.stderr.strip(), flush=True)
    assert result.returncode == 0, args
    return result.stdout.strip()

def restore_keyboard():
    subprocess.run(['ibus', 'engine', 'xkb:us::eng'], capture_output=True, timeout=20)

atexit.register(restore_keyboard)

def process_owners(name):
    # Some /proc projections enumerate task IDs as well as process leaders.
    # A new worker thread must not look like a compositor/panel restart.
    leaders = set()
    for pid in run('pidof', name).split():
        try:
            tasks = [int(item.name) for item in Path('/proc/' + pid + '/task').iterdir()
                     if item.name.isdigit()]
        except FileNotFoundError:
            continue
        # This legacy status projection prints each task's Pid as Tgid.
        # The task directory uses the real group; its initial thread has the
        # lowest ID during this bounded test (there is no PID wrap here).
        assert tasks, ('missing task group', pid)
        leaders.add(min(tasks))
    assert leaders, ('desktop component missing', name)
    return leaders

owners = {name: process_owners(name) for name in ('labwc', 'xfdesktop', 'xfce4-panel')}
assert run('gsettings', 'get', 'org.freedesktop.ibus.general', 'use-system-keyboard-layout') == 'true'
# A fresh IBus session may not have selected a global engine yet. Selecting
# the keyboard layout is a user operation, not a precondition on daemon state.
run('ibus', 'engine', 'xkb:us::eng')
assert 'rime' in run('ibus', 'list-engine')
with open('/dev/console', 'wb', buffering=0) as output:
    settings = subprocess.Popen(['ibus-setup'], stdout=output, stderr=output)
    time.sleep(5)
    assert settings.poll() is None, ('ibus-setup exited', settings.returncode)
    settings.terminate()
    settings.wait(timeout=10)
print('INPUT_METHOD: settings window opened and closed', flush=True)
run('ibus', 'engine', 'rime')
assert run('ibus', 'engine') == 'rime'

window = Gtk.Window(title='Rime Chinese input regression')
window.set_default_size(500, 150)
entry = Gtk.Entry()
window.add(entry)
window.show_all()
window.present()
entry.grab_focus()
started = time.monotonic()
ready = False
context = GLib.MainContext.default()
while time.monotonic() - started < 90:
    while context.pending():
        context.iteration(False)
    if not ready and time.monotonic() - started > 5:
        print('INPUT_METHOD: READY type nihao', flush=True)
        ready = True
    if entry.get_text() == '你好':
        Path('/tmp/a20-chinese-input.txt').write_text(entry.get_text())
        assert Path('/tmp/a20-chinese-input.txt').read_text() == '你好'
        for name, pids in owners.items():
            assert process_owners(name) == pids, ('desktop component exited or restarted', name)
        restore_keyboard()
        atexit.unregister(restore_keyboard)
        print('INPUT_METHOD: PASS Chinese text saved; desktop processes unchanged', flush=True)
        break
    time.sleep(0.02)
else:
    raise AssertionError(('Chinese input not committed', entry.get_text()))
window.destroy()
