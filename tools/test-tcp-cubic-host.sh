#!/bin/sh
# Host unit tests for the lwIP CUBIC implementation.
#
# Compiles tools/test-tcp-cubic-host.c, which #includes the real
# kernel/external/lwip/src/core/tcp_cubic.c against a mirror of the struct
# tcp_pcb fields that file touches.  So a passing run exercises the shipped
# source, not a copy of it.
#
# Kept out of `make dev-build` on purpose: it needs a host cc and libm, and the
# kernel build must not depend on either.  Run it directly, or via
# `make test-tcp-cubic`.
set -e

cd "$(dirname "$0")/.."

LWIP=kernel/external/lwip/src
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# The include path deliberately puts tools/tcp-cubic-test/lwip_port FIRST and
# does NOT add kernel/net/lwip_port at all: that directory ships freestanding
# <stdio.h>/<stdlib.h>/<string.h> shims which would shadow the host libc ones
# and compile the test against the kernel's own printf.  See the comment in
# tools/tcp-cubic-test/lwip_port/lwipopts.h.
#
# -Ikernel/include is here for one header: pcb_lane.h (an A20OS divergence from
# upstream, reached through tcp_priv.h) includes net/net_lane.h.  That header is
# pure macro definitions over CONFIG_NET_LANES, so it compiles on the host.
${CC:-cc} -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -Itools/tcp-cubic-test/lwip_port \
    -Itools/tcp-cubic-test \
    -Ikernel/include \
    -Ikernel \
    -I"$LWIP/include" \
    -I"$LWIP/core" \
    -o "$OUT/test-cubic" \
    tools/test-tcp-cubic-host.c \
    -lm

"$OUT/test-cubic"
