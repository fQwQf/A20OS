#!/usr/bin/env python3
"""Exercise production UDP peer mutations and heartbeat polling with sparse tables."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root / "kernel/cluster/udp.c")
    parser.add_argument("--limit", type=int, action="append")
    args = parser.parse_args()
    limits = args.limit or [4, 65, 256, 4096]
    if any(limit < 4 or limit > 4096 for limit in limits):
        parser.error("limits must be in [4, 4096]")
    source = args.source.read_text()
    fixture = (root / "tools/tests/clx_udp_peers_fixture.c.in").read_text()
    struct = re.search(r"typedef struct clx_udp_peer \{.*?} clx_udp_peer_t;", source, re.S)
    if not struct:
        raise RuntimeError("missing production peer type")
    fixture = fixture.replace("/* PRODUCTION_PEER_TYPE */", struct[0])
    # Use actual protocol/state values, including DOWN=0, rather than shadow enums.
    defines = []
    for header, names in (
        ("kernel/include/abi/native/types.h", ["A20_CLX_LINK_DOWN", "A20_CLX_LINK_SUSPECT",
                                             "A20_CLX_LINK_UP", "A20_CLX_TRANSPORT_UDP"]),
        ("kernel/cluster/uart.h", ["A20_CLX_TYPE_HELLO", "A20_CLX_TYPE_PING"]),
    ):
        text = (root / header).read_text()
        for name in names:
            match = re.search(r"^#define " + name + r"\s+[^\n]+", text, re.M)
            if not match:
                raise RuntimeError("missing production constant: " + name)
            defines.append(match[0])
    for name in ("UDP_RX_BUDGET", "UDP_PING_MS"):
        defines.append(re.search(r"^#define " + name + r"\s+[^\n]+", source, re.M)[0])
    fixture = fixture.replace("/* PRODUCTION_CONSTANTS */", "\n".join(defines))
    # Extract the bitmap size when present. The legacy source can also execute as
    # a negative control: it fails the measured sparse-poll lock budget first.
    words = re.search(r"^#define CLX_UDP_PEER_WORDS\s+[^\n]+", source, re.M)
    fixture = fixture.replace("/* PRODUCTION_BITMAP_SIZE */", words[0] if words else
                              "#define CLX_UDP_PEER_WORDS ((CLX_LIMIT_ROUTES + 63u) / 64u)")
    for name in ("a20_clx_udp_peer_add", "a20_clx_udp_peer_del", "udp_poll"):
        match = re.search(r"^(?:static )?(?:void|int) " + name + r"\([^)]*\)\s*\{.*?^}\n",
                          source, re.M | re.S)
        marker = "/* PRODUCTION_" + name + " */"
        if not match or fixture.count(marker) != 1:
            raise RuntimeError("missing production function or fixture splice: " + name)
        fixture = fixture.replace(marker, match[0])
    directory = root / "build/tmp"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="clx-udp-peers-", dir=directory) as tmp:
        test = Path(tmp) / "test.c"
        test.write_text(fixture)
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        for limit in limits:
            binary = Path(tmp) / ("test-" + str(limit))
            subprocess.run(compiler + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                       "-DCLX_LIMIT_ROUTES=" + str(limit), str(test), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True, timeout=60)
    print("check-cluster-udp-peers: PASS (production CRUD/poll, sparse/full/churn, ASan/UBSan)")


if __name__ == "__main__":
    main()
