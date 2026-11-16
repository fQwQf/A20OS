#!/usr/bin/env python3
"""Exercise unmodified OpenRC service supervision and FIFO control in guest."""
import glob
import os
import signal
import stat
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

    def replacement(parent, previous):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            child = child_of(parent)
            if child and child != previous:
                return child
            time.sleep(.05)
        raise AssertionError(('child was not respawned', parent, previous))

    control = '/run/openrc/supervise-' + name + '.ctl'
    started = False
    try:
        run('start')
        started = True
        supervisor = int(open(pidfile).read())
        first = child_of(supervisor)
        assert first and first != supervisor
        os.kill(first, signal.SIGKILL)
        second = replacement(supervisor, first)
        assert stat.S_ISFIFO(os.stat(control).st_mode)
        # Upstream --signal writes "sig N" to the control FIFO; --stop uses
        # SIGTERM on the supervisor, so rc-service stop alone cannot prove FIFO.
        subprocess.run(['/sbin/supervise-daemon', name, '--signal', 'KILL'],
                       env=dict(os.environ, RC_SVCNAME=name),
                       check=True, timeout=5)
        third = replacement(supervisor, second)
        print('OPENRC_SUPERVISION: supervisor=%d children=%d,%d,%d control=%s' %
              (supervisor, first, second, third, control), flush=True)
        run('status')
        run('stop')
        started = False
        assert not os.path.exists(pidfile)
        assert child_of(supervisor) is None
        assert not os.path.exists(control)
        print('OPENRC_SUPERVISION: PASS start, child crash/respawn, FIFO signal, status, stop', flush=True)
    finally:
        if started:
            subprocess.run(['/sbin/rc-service', name, 'stop'], timeout=15)
        os.unlink(service)


if __name__ == '__main__':
    main()
