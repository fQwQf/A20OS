#!/bin/sh
# Host unit tests for the INET socket lifecycle (kernel/net/backends/lwip/socket_inet_life.rs):
# pcb creation per type, the listen conversion and the teardown.  The primitives
# are stubbed inside the test module.
set -eu
cd "$(dirname "$0")/../.."

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/backends/lwip/socket_inet_life.rs -o "$tmpdir/socket-inet-life-tests"
"$tmpdir/socket-inet-life-tests"
