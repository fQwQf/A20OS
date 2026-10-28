#!/bin/sh
# Host unit tests for the bottom-half event ring and its per-socket consumer
# (kernel/net/backends/lwip/socket_inet_bh.rs): the scheduling gate, the three receive
# callbacks' staging policy, the consumer's flag ordering and drain bits, and the
# per-socket wrapper.  The primitives are stubbed inside the test module, so no
# kernel state is needed; see the module comment for what is Rust's and what C's.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/backends/lwip/socket_inet_bh.rs -o "$tmpdir/socket-inet-bh-tests"
"$tmpdir/socket-inet-bh-tests"
