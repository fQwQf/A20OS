#!/usr/bin/env python3
"""Host side of tools/mk/targets-smoke.mk:smoke-net-udp-hostfwd.

Sends 16 sequenced datagrams to the guest's forwarded UDP port and checks that
each comes back with the guest's one-byte transform.  UDP is lossy, so each
sequence number is retried within a deadline; a duplicate reply is tolerated
but a wrong payload or a sequence that never returns is a failure.
"""

import socket
import struct
import sys
import time

DEFAULT_PORT = 18085
TRANSACTIONS = 16
PAYLOAD_LEN = 512
DEADLINE_S = 40.0


def pattern(pos: int, seq: int) -> int:
    return (pos * 17 + seq * 29 + 0x3D) & 0xFF


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    s.connect(("127.0.0.1", port))
    deadline = time.time() + DEADLINE_S
    done = set()
    attempts = 0
    while len(done) < TRANSACTIONS and time.time() < deadline:
        for seq in range(TRANSACTIONS):
            if seq in done:
                continue
            payload = bytes(pattern(i, seq) for i in range(PAYLOAD_LEN))
            msg = struct.pack("<IH", seq, PAYLOAD_LEN) + payload
            try:
                s.send(msg)
                attempts += 1
            except OSError:
                continue
            try:
                reply, _ = s.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError as exc:
                sys.stderr.write(f"net-udp-host-probe: recv failed: {exc}\n")
                break
            if len(reply) != len(msg):
                sys.stderr.write(
                    f"net-udp-host-probe: wrong length {len(reply)} != {len(msg)}\n"
                )
                return 1
            rseq, rlen = struct.unpack_from("<IH", reply, 0)
            if rseq != seq or rlen != PAYLOAD_LEN:
                sys.stderr.write(
                    f"net-udp-host-probe: wrong header seq={rseq} len={rlen}\n"
                )
                return 1
            for i in range(PAYLOAD_LEN):
                if reply[6 + i] != (pattern(i, seq) ^ 0xA5):
                    sys.stderr.write(
                        f"net-udp-host-probe: wrong payload seq={seq} pos={i}\n"
                    )
                    return 1
            done.add(seq)
    s.close()
    if len(done) == TRANSACTIONS:
        print(
            f"net-udp-host-probe: completed {len(done)} datagram(s) "
            f"after {attempts} send(s)"
        )
        return 0
    sys.stderr.write(
        f"net-udp-host-probe: FAILED {len(done)}/{TRANSACTIONS} after "
        f"{attempts} send(s)\n"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
