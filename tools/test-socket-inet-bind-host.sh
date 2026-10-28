#!/bin/sh
# Host unit tests for the INET bind transaction (kernel/net/backends/lwip/socket_inet_bind.rs):
# the per-type dispatch, the lane entry/store, the pcb check and the status
# mapping.  The primitives are stubbed inside the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/backends/lwip/socket_inet_bind.rs -o "$tmpdir/socket-inet-bind-tests"
"$tmpdir/socket-inet-bind-tests"
