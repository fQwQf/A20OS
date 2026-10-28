#!/bin/sh
# Host unit tests for the UDP and raw send transactions
# (kernel/net/backends/lwip/socket_inet_send.rs): the ceiling, the lazy bind, the destination
# resolution and stale-peer drop, the local-delivery choice and the stack-path
# ordering.  The primitives are stubbed inside the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/backends/lwip/socket_inet_send.rs -o "$tmpdir/socket-inet-send-tests"
"$tmpdir/socket-inet-send-tests"
