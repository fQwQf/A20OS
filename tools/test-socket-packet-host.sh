#!/bin/sh
# Host unit tests for the AF_PACKET state machine (kernel/net/socket/socket_packet.rs):
# the capture gate and ring protocol, the delivery filter and payload slicing,
# the bound-socket census, and the bind and send transactions.  The primitives
# are stubbed inside the test module, so no kernel state is needed; see the
# module comment for what is Rust's and what is C's.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket/socket_packet.rs -o "$tmpdir/socket-packet-tests"
"$tmpdir/socket-packet-tests"
