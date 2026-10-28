#!/bin/sh
# Host unit tests for the Rust sendto transaction (kernel/net/socket/socket_sendto.rs):
# the early refusals, the family dispatch ladder, the EPIPE-versus-ENOTCONN rule
# for a stream with no pcb, the destination resolution and its four error
# answers, the queue-full handoff, the autobind guard and the chunk loop.  The
# primitives are stubbed in the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket/socket_sendto.rs -o "$tmpdir/socket-sendto-tests"
"$tmpdir/socket-sendto-tests"
