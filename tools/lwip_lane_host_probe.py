#!/usr/bin/env python3
"""Drive simultaneous TCP flows through QEMU hostfwd into distinct lwIP lanes."""
from __future__ import annotations

import argparse
import concurrent.futures
import socket
import time
from pathlib import Path

GUEST_IP = 0x0F02000A  # little-endian u32 for the wire bytes 10.0.2.15.
TRANSFER_BYTES = 64 * 1024
READY = "NET_LANE_HOSTFWD_READY:"


def lane_for(port: int, lanes: int) -> int:
    mask = 0xFFFFFFFF
    h = (port * 2654435761) & mask
    h ^= GUEST_IP
    h ^= h >> 15
    h = (h * 2246822519) & mask
    h ^= h >> 13
    return h % lanes


def payload() -> bytes:
    return bytes((i * 37 + (i >> 7) + 0x5B) & 0xFF for i in range(TRANSFER_BYTES))


def fnv1a(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def wait_ready(log_path: Path, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            text = log_path.read_text(errors="replace")
        except FileNotFoundError:
            text = ""
        for line in text.splitlines():
            if line.startswith(READY):
                return line
        if text and ("NET_LANE_HOSTFWD_TEST: FAIL" in text or "Kernel panic" in text):
            raise RuntimeError("guest failed before listeners were ready")
        time.sleep(0.05)
    raise TimeoutError(f"guest did not print listener READY within {timeout:g}s")


def recv_exact(sock: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        part = sock.recv(size - len(result))
        if not part:
            raise RuntimeError(f"peer closed after {len(result)} of {size} echoed bytes")
        result.extend(part)
    return bytes(result)


def run_one(host_port: int, expected: bytes) -> tuple[int, int]:
    with socket.create_connection(("127.0.0.1", host_port), timeout=15) as sock:
        sock.settimeout(20)
        send_error: list[BaseException] = []

        def sender() -> None:
            try:
                sock.sendall(expected)
                sock.shutdown(socket.SHUT_WR)
            except BaseException as exc:  # surfaced in the coordinating thread
                send_error.append(exc)

        import threading

        writer = threading.Thread(target=sender, daemon=True)
        writer.start()
        echoed = recv_exact(sock, len(expected))
        writer.join(timeout=5)
        if writer.is_alive():
            raise TimeoutError("host send thread did not finish")
        if send_error:
            raise send_error[0]
        if echoed != expected:
            raise RuntimeError(f"hostfwd port {host_port} returned corrupt TCP payload")
        return len(echoed), fnv1a(echoed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path, help="live QEMU serial log")
    parser.add_argument("--ports", required=True, help="comma-separated hostfwd ports")
    parser.add_argument("--lanes", type=int, choices=(1, 4), default=4)
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args()
    ports = [int(item) for item in args.ports.split(",")]
    if len(ports) != 4 or len(set(ports)) != 4 or any(not 1 <= p <= 65535 for p in ports):
        parser.error("--ports must contain four distinct valid ports")
    lane_map = [lane_for(port, args.lanes) for port in ports]
    if args.lanes == 4 and set(lane_map) != set(range(4)):
        parser.error(f"ports must cover all four guest lanes; got {list(zip(ports, lane_map))}")

    ready = wait_ready(args.log, args.timeout)
    if any(f"port={port}" not in ready for port in ports):
        raise RuntimeError(f"guest READY ports do not match requested ports: {ready}")
    data = payload()
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(ports)) as pool:
        results = list(pool.map(lambda p: run_one(p, data), ports))
    if any(size != TRANSFER_BYTES for size, _ in results):
        raise RuntimeError(f"short host transfer: {results}")
    digest_sum = sum(digest for _, digest in results) & 0xFFFFFFFFFFFFFFFF
    print(
        f"NET_LANE_HOST_PROBE: PASS connections={len(ports)} lanes={','.join(map(str, lane_map))} "
        f"bytes={TRANSFER_BYTES * len(ports)} digest_sum={digest_sum:016x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
