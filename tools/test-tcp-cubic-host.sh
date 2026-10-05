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

LOG="$OUT/run.log"
if ! "$OUT/test-cubic" > "$LOG" 2>&1; then
    cat "$LOG"
    echo "test-tcp-cubic-host: FAILED (see above)" >&2
    exit 1
fi
cat "$LOG"

# The exit status alone does not prove the suite still has the coverage it is
# cited for: a test that is renamed, or dropped from main() while its function
# stays in the file behind it, keeps the binary exiting 0.  So the run is also
# required to have actually executed every clause this port claims to implement,
# and to have finished clean.
missing=
for label in \
    "test_icbrt64" \
    "test_parse" \
    "test_loss_response" \
    "test_fast_convergence" \
    "test_rto_response" \
    "test_slow_start" \
    "test_epoch_from_slow_start" \
    "test_curve_value" \
    "test_tcp_friendly" \
    "test_friendly_region_binding" \
    "test_growth_curve" \
    "test_beats_reno" \
    "test_idle_restart" \
    "test_no_overflow"
do
    grep -q "^$label" "$LOG" || missing="$missing $label"
done
if [ -n "$missing" ]; then
    echo "test-tcp-cubic-host: FAILED -- these tests did not run:$missing" >&2
    exit 1
fi
if ! grep -qE '^=== [0-9]+ checks, 0 failures ===$' "$LOG"; then
    echo "test-tcp-cubic-host: FAILED -- the run did not end with a clean tally" >&2
    exit 1
fi

# Guard on the fixture the RFC 8312 4.2 tests rest on.  sa_rtt_seconds() reads
# TCP_SLOW_INTERVAL and fresh_pcb() gives the pcb a small whole number of ticks,
# so if a port change moved the tick period enough to make sa == 1 a long RTT the
# friendly-region test would quietly stop exercising a short-RTT case and still
# pass.  One tick has to stay sub-second for the test to mean what it says.
tick_s=$(awk '/one tcp_ticks =/ { for (i = 1; i <= NF; i++) if ($i == "s,") print $(i - 1) }' \
             "$LOG" | tail -n 1)
if [ -z "$tick_s" ]; then
    echo "test-tcp-cubic-host: FAILED -- could not read the tick period from the run" >&2
    exit 1
fi
if ! awk -v t="$tick_s" 'BEGIN { exit !(t > 0.0 && t < 1.0) }'; then
    echo "test-tcp-cubic-host: FAILED -- one tcp_ticks is ${tick_s}s, but the" \
         "RFC 8312 4.2 tests assume a sub-second tick" >&2
    exit 1
fi

echo "test-tcp-cubic-host: PASS (every clause test ran, 1 tick = ${tick_s}s)"
