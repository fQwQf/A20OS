#!/bin/sh
# Host unit tests for the Rust accept transaction (kernel/net/socket/socket_accept.rs):
# the flag validation and the type/domain refusal, the child handback with its
# peer address and nonblock inheritance, the closed/nonblock answers, the
# waiting loop's park decisions, and the install-failure rollback order with its
# three reference-drop cases.  The primitives are stubbed in the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket/socket_accept.rs -o "$tmpdir/socket-accept-tests"
"$tmpdir/socket-accept-tests"
