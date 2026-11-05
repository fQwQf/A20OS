#!/bin/sh
# Host unit tests for the Rust blocking receive transaction
# (kernel/net/socket/socket_recv.rs): the WAITALL loop, the PEEK/TRUNC answers, stream
# coalescing versus the datagram one-shot, the EOF and nonblocking ordering, the
# timeout and park decisions, and the channel path's legacy-first drain,
# credential stamping and -EINTR.  The primitives are stubbed in the test module.
set -eu
cd "$(dirname "$0")/../.."

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket/socket_recv.rs -o "$tmpdir/socket-recv-tests"
"$tmpdir/socket-recv-tests"
