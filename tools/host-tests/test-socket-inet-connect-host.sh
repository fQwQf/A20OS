#!/bin/sh
# Host unit tests for the local TCP connect pairing transaction
# (kernel/net/backends/lwip/socket_inet_connect.rs): the ephemeral bind, the shortcut gate, the
# child inheritance, the register-before-push order and the rollback paths.  The
# primitives are stubbed inside the test module, so no kernel state is needed.
set -eu
cd "$(dirname "$0")/../.."

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/backends/lwip/socket_inet_connect.rs -o "$tmpdir/socket-inet-connect-tests"
"$tmpdir/socket-inet-connect-tests"
