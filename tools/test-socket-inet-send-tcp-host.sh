#!/bin/sh
# Host unit tests for the TCP stream send transaction
# (kernel/net/socket_inet_send_tcp.rs): the refusals, the local path, the room
# computation, the write retry, the wait policy and the status mapping.  The
# primitives are stubbed inside the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket_inet_send_tcp.rs -o "$tmpdir/socket-inet-send-tcp-tests"
"$tmpdir/socket-inet-send-tcp-tests"
