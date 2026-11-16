#!/usr/bin/python3
"""Mirror session stderr to the persistent kernel log, without console spam."""
import os
import select
import signal
import subprocess
import sys
import time


def supervise(command, path, kmsg='/dev/kmsg', stop_timeout=3.0):
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
    stop_signal = None
    old_handlers = {}

    def request_stop(signum, frame):
        nonlocal stop_signal
        # Only publish the request here: logging and child control run in the
        # normal loop, outside an interrupted file write/select operation.
        if stop_signal is None:
            stop_signal = signum

    def signal_child(signum):
        if child is not None and child.poll() is None:
            try:
                os.killpg(child.pid, signum)
            except ProcessLookupError:
                child.send_signal(signum)

    try:
        for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            old_handlers[signum] = signal.signal(signum, request_stop)
        if log_error is not None:
            record(('session file unavailable errno=' + str(log_error)).encode())
        if stop_signal is not None:
            return 128 + stop_signal
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        record(('session child started pid=' + str(child.pid)).encode())
        pending = b''
        finished_at = None
        stop_deadline = None
        forced = False
        pipe_open = True
        while True:
            if stop_signal is not None and stop_deadline is None:
                record(('session stop requested signal=' + str(stop_signal)).encode())
                signal_child(stop_signal)
                # Leave time for reaping and flushing within OpenRC's TERM/5.
                stop_deadline = time.monotonic() + stop_timeout
            if stop_deadline is not None and not forced and time.monotonic() >= stop_deadline:
                if child.poll() is None:
                    record(b'session stop timed out; sending SIGKILL')
                    signal_child(signal.SIGKILL)
                forced = True
            readable, _, _ = select.select([child.stdout] if pipe_open else [], [], [], 0.2)
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
                    pipe_open = False
            if child.poll() is not None:
                # Detached descendants may retain stdout after the session
                # owner exits. Do not wait for their pipe EOF indefinitely.
                if finished_at is None:
                    finished_at = time.monotonic()
                if not pipe_open or time.monotonic() - finished_at >= 1:
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
        # Unexpected logger errors must not leave its direct session owner
        # running without supervision. Never signal a reused/reaped PID.
        try:
            if child is not None and child.poll() is None:
                signal_child(signal.SIGKILL)
                child.wait(timeout=1)
        finally:
            if child is not None and child.stdout is not None:
                child.stdout.close()
            log.close()
            if kernel >= 0:
                os.close(kernel)
            for signum, handler in old_handlers.items():
                signal.signal(signum, handler)


if __name__ == '__main__':
    raise SystemExit(supervise(sys.argv[2:], sys.argv[1]))
