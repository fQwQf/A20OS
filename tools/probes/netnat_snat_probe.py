#!/usr/bin/env python3
"""Host peer for tools/mk/targets-smoke.mk:smoke-netfilter-snat.

The guest initiates to 10.0.2.2:<port> (QEMU's host alias) and this peer
listens on the host loopback equivalent.  It echoes one byte per connection
and counts the complete transactions.  It does not assert the guest's source
port: QEMU's user-mode network rewrites the outer tuple, so that observation
belongs to the guest's own /proc/a20/netfilter check, not here.
"""

import socket
import sys
import time

DEFAULT_PORT = 18083
DEFAULT_TRANSACTIONS = 17
DEADLINE_S = 60.0


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    transactions = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_TRANSACTIONS
    if transactions <= 0 or transactions > 1024:
        sys.stderr.write("netnat-snat-probe: transaction count must be 1..1024\n")
        return 2

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    srv.settimeout(1.0)
    deadline = time.time() + DEADLINE_S
    completed = 0
    while completed < transactions:
        if time.time() >= deadline:
            break
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError as exc:
            sys.stderr.write(f"netnat-snat-probe: accept failed: {exc}\n")
            break
        conn.settimeout(5.0)
        try:
            while completed < transactions:
                data = conn.recv(1)
                if not data:
                    break
                if data == b"S":
                    conn.sendall(data)
                    completed += 1
        except OSError:
            pass
        finally:
            conn.close()
    srv.close()
    if completed == transactions:
        print(f"netnat-snat-probe: completed {completed} echo transaction(s)")
        return 0
    sys.stderr.write(
        f"netnat-snat-probe: FAILED after {completed}/{transactions} transaction(s)\n"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
