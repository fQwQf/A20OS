#!/bin/sh
# Host unit tests for the Rust poll readiness computation
# (kernel/net/socket/socket_poll.rs): the non-socket refusal, the channel arm's
# readable/writable/error rules, the legacy arm's HUP precedence and pending
# accept, the connecting socket, and the full-peer-queue rule with its weakly
# consistent peer read.  The primitives are stubbed in the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket/socket_poll.rs -o "$tmpdir/socket-poll-tests"
"$tmpdir/socket-poll-tests"
