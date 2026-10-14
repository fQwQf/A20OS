#!/usr/bin/env python3
"""Compatibility contract for the UXFS flush capability handshake."""
import struct


def old_kernel_v1(args, memory, capacity=0x123456789ABCDEF0):
    version, write, count, buf = args
    if version == 0 or version > 1:
        return -22
    if count == 0:
        # Legacy syscall treated every write value as a capacity query.
        memory[buf:buf + 8] = struct.pack("<Q", capacity)
        return 0
    return -22


def test_v2_capability_request_fails_closed_on_v1_kernel():
    memory = bytearray(b"\xa5" * 32)
    query = (2, 2, 0, 8)
    assert old_kernel_v1(query, memory) == -22
    assert memory[8:16] == b"\xa5" * 8
    assert memory[16:] == b"\xa5" * 16


def test_v1_capacity_alias_is_not_a_capability_response():
    memory = bytearray(16)
    assert old_kernel_v1((1, 2, 0, 0), memory) == 0
    response = struct.unpack("<Q", memory[:8])[0]
    assert response != 0x5546585300000001


def test_capability_response_has_fixed_magic_and_width():
    response = 0x5546585300000001
    assert len(struct.pack("<Q", response)) == 8
    assert response == 0x5546585300000000 | 1


if __name__ == "__main__":
    test_v2_capability_request_fails_closed_on_v1_kernel()
    test_v1_capacity_alias_is_not_a_capability_response()
    test_capability_response_has_fixed_magic_and_width()
    print("UXFS flush ABI compatibility: 3/3 PASS")
