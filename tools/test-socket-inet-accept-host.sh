#!/bin/sh
# Host unit tests for the staged-accept state machine
# (kernel/net/socket_inet_accept.rs): the publish transaction, the drain with its
# adoption order and rollbacks, the single-consumer claim, and the purge walk.
# The primitives are stubbed inside the test module, so no kernel state is
# needed; see the module comment for what is Rust's and what is C's.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket_inet_accept.rs -o "$tmpdir/socket-inet-accept-tests"
"$tmpdir/socket-inet-accept-tests"
