#!/bin/sh
# Host unit tests for the NAT rewrite half of netfilter.
#
# Compiles tools/test-nat-rewrite-host.c together with the real
# kernel/net/netfilter_rewrite.c, so a passing run exercises the shipped source
# rather than a copy of it.  The test supplies its own a20_lwip_netif_ipv4(),
# which is the only kernel symbol that file needs; everything else in it is
# pure.
#
# Kept out of `make dev-build` on purpose, exactly like
# tools/test-tcp-cubic-host.sh: it needs a host cc, and the kernel build must
# not depend on one.  Run it directly, or via `make test-nat-rewrite`.
set -e

cd "$(dirname "$0")/.."

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# -Ikernel/include only: netfilter_rewrite.h pulls in core/types.h, which is
# built on compiler-provided integer types, so nothing here drags a freestanding
# kernel <stdint.h> shim over the host one.
${CC:-cc} -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -Ikernel/include \
    -o "$OUT/test-nat-rewrite" \
    tools/test-nat-rewrite-host.c \
    kernel/net/netfilter_rewrite.c

"$OUT/test-nat-rewrite"
