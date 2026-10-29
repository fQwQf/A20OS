#!/usr/bin/env python3
"""Independent checksums against the production pristine-port completion code."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile


def checksum(data):
    if len(data) & 1:
        data += b'\0'
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return ~total & 65535


with tempfile.TemporaryDirectory() as directory:
    library = Path(directory) / 'completion.so'
    root = Path(__file__).resolve().parents[1]
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    str(root / 'kernel/net/backends/lwip/software_tx.c'), '-o', str(library)], check=True)
    complete = ctypes.CDLL(str(library)).a20_lwip_complete_software_tx
    complete.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
    complete.restype = ctypes.c_int
    cases = 0
    for protocol, header_size, check_offset in [(6, 20, 16), (17, 8, 6), (1, 8, 2)]:
        for payload in [b'', b'a', bytes(range(63)), bytes(range(256)) * 2]:
            for options in [b'', b'\x01\x01\x01\x01']:
                transport = bytearray(header_size) + payload
                if protocol == 17:
                    struct.pack_into('!H', transport, 4, len(transport))
                ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45 + len(options)//4, 0,
                    20 + len(options) + len(transport), 5, 0, 63, protocol, 0,
                    bytes([192, 0, 2, 1]), bytes([198, 51, 100, 2]))) + options
                wire = bytes(12) + b'\x08\x00' + ip + transport
                buf = ctypes.create_string_buffer(wire, len(wire))
                assert complete(buf, len(wire)) == 0
                result = bytes(buf)
                header = result[14:34 + len(options)]
                segment = result[34 + len(options):]
                pseudo = bytes(ip[12:20]) + bytes([0, protocol]) + struct.pack('!H', len(segment))
                assert checksum(header) == 0
                assert checksum((pseudo if protocol != 1 else b'') + segment) == 0
                assert segment[:check_offset] == transport[:check_offset]
                assert segment[check_offset + 2:] == transport[check_offset + 2:]
                for flags in (0x2000, 1):
                    fragment = bytearray(wire)
                    struct.pack_into('!H', fragment, 20, flags)
                    buf = ctypes.create_string_buffer(bytes(fragment), len(fragment))
                    assert complete(buf, len(fragment)) == -1
                    assert bytes(buf) == fragment
                cases += 1
    # Every strict prefix must be safe, including empty Ethernet/IP packets.
    for length in range(len(wire)):
        buf = ctypes.create_string_buffer(wire[:length], max(1, length))
        complete(buf, length)
    print(f'SOFTWARE_TX_PASS cases={cases}, fragment rejection and truncated prefixes')
