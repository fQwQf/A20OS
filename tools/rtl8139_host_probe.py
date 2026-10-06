#!/usr/bin/env python3
"""Host side of tools/targets-smoke.mk:smoke-net-rtl8139.

Connects to a QEMU hostfwd port, sends one byte, and requires it back.

Same probe shape as tools/e1000_host_probe.py and tools/netnat_host_probe.py --
connect, one byte out, one byte back -- because the question asked from the host
side is identical for every NIC gate in this tree: did a frame make the round
trip through the guest.  What differs is what the gate concludes.  There the echo
is only traffic to make an interrupt fire, and the assertion is the driver's
counter; here the echo is the assertion itself, and the interrupt counters are
corroboration.  The round trip is what the gate was asked to prove, so it is what
this probe refuses to be vague about.

Retries, because the guest needs to boot, enumerate a PCI function, program a
64 KiB ring and open a listener before a connection can land; a refused
connection before that is a timing artefact and not a verdict.  Fails after the
deadline rather than reporting what it did not see -- a probe that gave up
quietly would let the gate go green on a port forward that never worked, which is
the one outcome this gate exists to prevent.
"""

import socket
import sys
import time

DEFAULT_PORT = 18093
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
                    "rtl8139-host-probe: connected and echoed after %d attempt(s)"
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
        "rtl8139-host-probe: FAILED after %d attempts: %s\n" % (attempts, last)
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())