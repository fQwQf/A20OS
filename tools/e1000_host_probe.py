#!/usr/bin/env python3
"""Host side of tools/targets-smoke.mk:smoke-net-e1000-irq.

Connects to a QEMU hostfwd port, sends one byte, and requires it back.

This is deliberately the same probe shape as tools/netnat_host_probe.py --
connect, one byte out, one byte back -- because it answers the same class of
question from the same side: did a frame make the round trip through the guest
NIC.  What differs is what the gate does with the answer.  There, the round
trip is the traffic source that makes the guest's e1000 receive interrupt fire
at all, and the interrupt counter is the assertion; here, the echo only has to
establish that inbound traffic exists and the outbound direction works too.

Retries, because a connection refused before the guest has booted and opened
its listener is a timing artefact and not a verdict.  Fails after the deadline
rather than reporting what it did not see: a probe that gave up quietly would
let the gate go green on a port forward that never worked, which is the one
outcome this gate exists to prevent.
"""

import socket
import sys
import time

DEFAULT_PORT = 18091
DEADLINE_S = 40.0


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    deadline = time.time() + DEADLINE_S
    last = "no attempt made"
    attempts = 0
    while time.time() < deadline:
        attempts += 1
        s = socket.socket()
        s.settimeout(2.0)
        try:
            s.connect(("127.0.0.1", port))
            s.sendall(b"A")
            if s.recv(1) == b"A":
                print(
                    "e1000-host-probe: connected and echoed after %d attempt(s)"
                    % attempts
                )
                return 0
            last = "connected but the echo byte did not come back"
        except OSError as exc:
            last = str(exc)
        finally:
            s.close()
        time.sleep(0.5)
    sys.stderr.write(
        "e1000-host-probe: FAILED after %d attempts: %s\n" % (attempts, last)
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())