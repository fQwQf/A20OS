#!/usr/bin/env python3
"""Host side of tools/targets-smoke.mk:smoke-netfilter-nat.

Connects to a QEMU hostfwd port, sends one byte, and requires it back.  The
echo is the point: the guest socket the connection lands on is listening on a
*different* port (the DNAT target), so a reply that arrives carrying the
forwarded source port can only have been rewritten on the way out.

Retries because the guest needs a few seconds to boot, to create its listener
and to install the NAT rule; a connection refused before that is a timing
artefact, not a verdict.  Fails after the deadline rather than reporting what it
did not see -- a probe that gave up quietly would let the gate go green on a
port forward that never worked.
"""

import socket
import sys
import time

DEFAULT_PORT = 18081
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
                    "netnat-host-probe: connected and echoed after %d attempt(s)"
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
        "netnat-host-probe: FAILED after %d attempts: %s\n" % (attempts, last)
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())