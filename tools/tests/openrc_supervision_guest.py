#!/usr/bin/env python3
"""Exercise unmodified OpenRC service supervision and FIFO control in guest."""
import glob
import os
import signal
import subprocess
import time


def main():
    name = 'a20-supervision-qa'
    service = '/etc/init.d/' + name
    pidfile = '/run/' + name + '.pid'
    assert not os.path.exists(service)
    with open(service, 'w') as output:
        output.write('''#!/sbin/openrc-run
supervisor="supervise-daemon"
command="/bin/sleep"
command_args="300"
pidfile="%s"
respawn_delay=1
respawn_max=3
respawn_period=30
retry="TERM/2/KILL/1"
''' % pidfile)
    os.chmod(service, 0o755)

    def run(action):
        subprocess.run(['/sbin/rc-service', name, action], check=True, timeout=15)

    def child_of(parent):
        for path in glob.glob('/proc/[0-9]*/stat'):
            try:
                fields = open(path).read().rsplit(')', 1)[1].split()
                if int(fields[1]) == parent and fields[0] != 'Z':
                    return int(path.split('/')[2])
            except (OSError, ValueError, IndexError):
                pass
        return None

    started = False
    try:
        run('start')
        started = True
        supervisor = int(open(pidfile).read())
        first = child_of(supervisor)
        assert first and first != supervisor
        os.kill(first, signal.SIGKILL)
        deadline = time.monotonic() + 8
        second = None
        while time.monotonic() < deadline:
            second = child_of(supervisor)
            if second and second != first:
                break
            time.sleep(.05)
        assert second and second != first, (supervisor, first, second)
        run('status')
        run('stop')
        started = False
        assert not os.path.exists(pidfile)
        assert child_of(supervisor) is None
        print('OPENRC_SUPERVISION: PASS start, child crash/respawn, status, FIFO stop', flush=True)
    finally:
        if started:
            subprocess.run(['/sbin/rc-service', name, 'stop'], timeout=15)
        os.unlink(service)


if __name__ == '__main__':
    main()
