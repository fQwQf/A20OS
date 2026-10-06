#!/usr/bin/env python3
"""Host side of tools/targets-smoke.mk:smoke-virtio-console.

The guest runs user/cmds/core/vport_test, which opens /dev/vport0 and prints
"VPORT_TEST: READY" once the driver is bound and the console mirror is
detached.  This probe waits for that marker in the QEMU serial log, then
connects to the virtconsole chardev socket and writes 64 bytes; the guest reads
them and writes the same 64 bytes back, which is what this probe asserts on.

Retries because the guest needs a few seconds to boot, to load
virtio-console.a20drv and to open the node -- a connection refused before that
is a timing artefact, not a verdict.  Fails loudly after the deadline rather
than reporting what it did not see: a probe that gave up quietly would let the
gate go green on a console port that never carried a byte.

usage: vport_host_probe.py SOCKET_PATH LOG_PATH [TIMEOUT_S]
"""

import socket
import sys
import time

PAYLOAD = bytes((ord("A") + (i % 26)) for i in range(64))
READY_MARKER = "VPORT_TEST: READY"
DEFAULT_TIMEOUT_S = 90.0


def wait_for_ready(log_path: str, deadline: float) -> bool:
    """True once the guest printed its READY marker, False on timeout.

    Re-reads from the start each poll: the gate redirects the QEMU log, so the
    marker can already be on disk by the time this runs.
    """
    while time.time() < deadline:
        try:
            with open(log_path, "r", errors="replace") as fp:
                if READY_MARKER in fp.read():
                    return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def recv_exactly(sock: socket.socket, want: int, deadline: float) -> bytes:
    buf = b""
    while len(buf) < want and time.time() < deadline:
        try:
            chunk = sock.recv(want - len(buf))
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
    return buf


def main() -> int:
    sock_path = sys.argv[1] if len(sys.argv) > 1 else ""
    log_path = sys.argv[2] if len(sys.argv) > 2 else ""
    timeout = float(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_TIMEOUT_S
    if not sock_path or not log_path:
        sys.stderr.write(
            "vport-host-probe: usage: vport_host_probe.py SOCKET LOG [TIMEOUT_S]\n"
        )
        return 2

    deadline = time.time() + timeout
    if not wait_for_ready(log_path, deadline):
        sys.stderr.write(
            "vport-host-probe: FAILED: guest never printed %r in %s\n"
            % (READY_MARKER, log_path)
        )
        return 1

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5.0)
    last = "no attempt made"
    try:
        s.connect(sock_path)
        s.sendall(PAYLOAD)
        got = recv_exactly(s, len(PAYLOAD), min(deadline, time.time() + 20.0))
        if got == PAYLOAD:
            print(
                "vport-host-probe: wrote %d bytes to %s and read the same %d back"
                % (len(PAYLOAD), sock_path, len(got))
            )
            return 0
        last = "echo mismatch: got %d bytes, %r" % (len(got), got)
    except OSError as exc:
        last = str(exc)
    finally:
        s.close()

    sys.stderr.write("vport-host-probe: FAILED: %s\n" % last)
    return 1


if __name__ == "__main__":
    sys.exit(main())
