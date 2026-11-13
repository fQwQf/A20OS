#!/usr/bin/python3
"""Mirror session stderr to the persistent kernel log, without console spam."""
import os
import select
import subprocess
import sys
import time


def supervise(command, path, kmsg='/dev/kmsg'):
    log_error = None
    try:
        log = open(path, 'ab', buffering=0)
    except OSError as error:
        log_error = error.errno
        log = open('/dev/null', 'ab', buffering=0)
    try:
        kernel = os.open(kmsg, os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        kernel = -1
    last_sync = 0.0

    def record(line):
        nonlocal last_sync
        line = line.decode('utf-8', 'replace').encode('utf-8')
        try:
            if log.tell() > 4 * 1024 * 1024:
                # Keep session diagnostics bounded even if a toolkit loops.
                # The kernel ring/ESP remains the independent record stream.
                log.truncate(0)
            log.write(line + b'\n')
            if time.monotonic() - last_sync >= 0.5:
                os.fsync(log.fileno())
                last_sync = time.monotonic()
        except OSError:
            pass
        if kernel >= 0:
            try:
                visible = line[:3000].decode('utf-8', 'replace').encode('utf-8')
                os.write(kernel, b'[DESKTOP] ' + visible + b'\n')
            except OSError:
                pass

    child = None
    try:
        if log_error is not None:
            record(('session file unavailable errno=' + str(log_error)).encode())
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        record(('session child started pid=' + str(child.pid)).encode())
        pending = b''
        finished_at = None
        while True:
            readable, _, _ = select.select([child.stdout], [], [], 0.2)
            if readable:
                data = os.read(child.stdout.fileno(), 4096)
                pending += data
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    record(line)
                if len(pending) >= 3000:
                    record(pending[:3000])
                    pending = pending[3000:]
                if not data:
                    break
            if child.poll() is not None:
                # Detached descendants may retain stdout after the session
                # owner exits. Do not wait for their pipe EOF indefinitely.
                if finished_at is None:
                    finished_at = time.monotonic()
                if not readable or time.monotonic() - finished_at >= 1:
                    break
        if pending:
            record(pending)
        result = child.wait()
        record(('session child exited status=' + str(result)).encode())
        try:
            os.fsync(log.fileno())
        except OSError:
            pass
        return 128 - result if result < 0 else result
    finally:
        if child is not None and child.stdout is not None:
            child.stdout.close()
        log.close()
        if kernel >= 0:
            os.close(kernel)


if __name__ == '__main__':
    raise SystemExit(supervise(sys.argv[2:], sys.argv[1]))
