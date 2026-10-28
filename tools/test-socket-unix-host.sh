#!/bin/sh
# Host unit tests for the AF_UNIX state machine (kernel/net/socket/socket_unix.rs): the
# address preparation, the bind rollback, the connect pairing and its rollback,
# the sendto destination resolution and fragmentation, and the channel data
# plane.  The primitives are stubbed inside the test module, so no kernel state
# is needed; see the module comment for what is Rust's and what is C's.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket/socket_unix.rs -o "$tmpdir/socket-unix-tests"
"$tmpdir/socket-unix-tests"
