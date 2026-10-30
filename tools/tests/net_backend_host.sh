#!/bin/sh
# Host test for kernel/net/core/backend.c.  No cross toolchain, no lwIP/BSD:
# the registry and capability validation are deliberately protocol-free.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/a20-net-backend-host
cc -std=gnu99 -fno-builtin -Wall -Wextra -Werror \
    -I "$ROOT/kernel/include" -I "$ROOT/kernel" \
    "$ROOT/tools/tests/net_backend_host.c" \
    "$ROOT/kernel/net/core/backend.c" \
    -o "$OUT"
"$OUT"
