#!/usr/bin/env python3
"""Guest-only Firefox startup probe for the BSD desktop image.

Runs the real Firefox binary directly (bypassing the /usr/bin wrapper), first
with --version and then with --marionette.  Prints a bounded process tree so a
startup stall can be attributed to the browser, a helper process, or the kernel
wait/exec path rather than to /proc/<pid>/wchan, which only reports "do_wait"
for every blocked task in A20OS.
"""
import os
import shlex
import shutil
import socket
import subprocess
import sys
import time

FIREFOX = '/usr/lib/firefox/firefox'
LOG = '/tmp/ffprobe.log'
PROFILE = '/tmp/ffprobe-profile'


def procs():
    rows = []
    for name in os.listdir('/proc'):
        if not name.isdigit():
            continue
        pid = int(name)
        try:
            with open(f'/proc/{pid}/stat', 'rb') as f:
                stat = f.read().decode(errors='replace')
            comm = stat[stat.find('(') + 1:stat.rfind(')')]
            rest = stat[stat.rfind(')') + 2:].split()
            state, ppid = rest[0], int(rest[1])
            with open(f'/proc/{pid}/status', 'rb') as f:
                status = f.read().decode(errors='replace')
            rss = next((x.split('\t')[1].strip() for x in status.splitlines()
                        if x.startswith('Rss:')), '?')
            rows.append((pid, ppid, state, comm, rss))
        except (FileNotFoundError, ProcessLookupError, IndexError, ValueError):
            continue
    return rows


def tree(root):
    rows = procs()
    selected = {root}
    changed = True
    while changed:
        changed = False
        for pid, ppid, *_ in rows:
            if ppid in selected and pid not in selected:
                selected.add(pid)
                changed = True
    return [r for r in rows if r[0] in selected or r[0] == root]


def show_tree(root):
    for pid, ppid, state, comm, rss in sorted(tree(root)):
        print(f'FFPROBE_PROC pid={pid} ppid={ppid} state={state} '
              f'rss={rss} comm={comm}', flush=True)


def dump_log():
    try:
        fd = os.open(LOG, os.O_RDONLY)
    except FileNotFoundError:
        print('FFPROBE_LOG missing', flush=True)
        return
    try:
        size = os.lseek(fd, 0, os.SEEK_END)
        start = max(0, size - 12000)
        os.lseek(fd, start, os.SEEK_SET)
        data = os.read(fd, size - start)
    finally:
        os.close(fd)
    print('FFPROBE_LOG_BEGIN', flush=True)
    sys.stdout.write(data.decode(errors='replace'))
    if data and not data.endswith(b'\n'):
        print(flush=True)
    print('FFPROBE_LOG_END', flush=True)


def port_open(port):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=1):
            return True
    except OSError:
        return False


def launch(label, argv, seconds, env=None, port=None):
    print(f'FFPROBE_LAUNCH {label} argv={argv!r}', flush=True)
    child_env = os.environ.copy()
    child_env.update(env or {})
    pidfile = f'/tmp/ffprobe-{label}.pid'
    try:
        os.unlink(pidfile)
    except FileNotFoundError:
        pass
    command = ' '.join(shlex.quote(x) for x in argv)
    shell = subprocess.Popen(
        ['/bin/sh', '-c',
         f'{command} >>{LOG} 2>&1 </dev/null & echo $! >{pidfile}'],
        env=child_env, start_new_session=True)
    print(f'FFPROBE_SPAWNED {label} shell={shell.pid}', flush=True)
    deadline = time.monotonic() + 10
    pid = None
    while time.monotonic() < deadline:
        try:
            with open(pidfile) as f:
                pid = int(f.read().strip())
            break
        except (FileNotFoundError, ValueError):
            time.sleep(0.2)
    if pid is None:
        print(f'FFPROBE_NOPID {label}', flush=True)
        dump_log()
        return False
    print(f'FFPROBE_PID {label} pid={pid}', flush=True)
    deadline = time.monotonic() + seconds
    connected = False
    while time.monotonic() < deadline:
        if not os.path.exists(f'/proc/{pid}'):
            print(f'FFPROBE_EXIT {label} pid={pid}', flush=True)
            break
        if port and port_open(port):
            connected = True
            print(f'FFPROBE_PORT {label} port={port} open', flush=True)
            break
        time.sleep(5)
        print(f'FFPROBE_STATE {label} elapsed={int(seconds - (deadline - time.monotonic()))}',
              flush=True)
    else:
        print(f'FFPROBE_TIMEOUT {label} connected={connected}', flush=True)
    dump_log()
    alive = True
    try:
        with open(f'/proc/{pid}/stat') as f:
            stat = f.read()
        alive = stat[stat.rfind(')') + 2:].split()[0] != 'Z'
    except (FileNotFoundError, IndexError):
        alive = False
    if alive:
        try:
            os.kill(pid, 9)
        except ProcessLookupError:
            pass
        time.sleep(2)
    return connected


def main():
    print('FFPROBE_BEGIN', flush=True)
    print('FFPROBE_UNAME', os.uname(), flush=True)
    profiles = '/root/.config/mozilla/firefox'
    if os.path.isdir(profiles):
        for name in os.listdir(profiles):
            directory = os.path.join(profiles, name)
            if not os.path.isdir(directory):
                continue
            for lock in ('lock', '.parentlock'):
                path = os.path.join(directory, lock)
                try:
                    os.unlink(path)
                    print(f'FFPROBE_UNLINK {path}', flush=True)
                except FileNotFoundError:
                    pass
    shutil.rmtree(PROFILE, ignore_errors=True)
    os.makedirs(PROFILE, exist_ok=True)

    # A direct exec of the ELF bypasses /usr/bin/firefox (a shell script whose
    # 45-byte form makes the kernel report [ELF] read header failed).
    launch('direct-marionette-headless',
           [FIREFOX, '--marionette', '--headless', '--no-remote',
            '--profile', PROFILE, 'about:blank'], 110, port=2828)
    shutil.rmtree(PROFILE, ignore_errors=True)
    os.makedirs(PROFILE, exist_ok=True)
    launch('direct-marionette-gui',
           [FIREFOX, '--marionette', '--no-remote', '--profile', PROFILE,
            'https://example.org/'], 110, port=2828)
    print('FFPROBE_DONE', flush=True)


if __name__ == '__main__':
    main()
