#!/usr/bin/env python3
"""Host side of tools/mk/targets-smoke.mk:smoke-netfilter-nat.

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
DEFAULT_TRANSACTIONS = 1
DEADLINE_S = 40.0


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    transactions = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_TRANSACTIONS
    if transactions <= 0 or transactions > 1024:
        sys.stderr.write("netnat-host-probe: transaction count must be 1..1024\n")
        return 2
    deadline = time.time() + DEADLINE_S
    last = "no attempt made"
    attempts = 0
    completed = 0
    while completed < transactions:
        if time.time() >= deadline:
            break
        attempts += 1
        s = socket.socket()
        s.settimeout(2.0)
        try:
            s.connect(("127.0.0.1", port))
            s.sendall(b"A")
            if s.recv(1) == b"A":
                completed += 1
                continue
            last = "connected but the echo byte did not come back"
            break
        except OSError as exc:
            last = str(exc)
        finally:
            s.close()
        time.sleep(0.5)
    if completed == transactions:
        print(
            "netnat-host-probe: completed %d echo transaction(s) after %d attempt(s)"
            % (completed, attempts)
        )
        return 0
    sys.stderr.write(
        "netnat-host-probe: FAILED after %d/%d transaction(s), %d attempt(s): %s\n"
        % (completed, transactions, attempts, last)
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
