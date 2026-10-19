#!/bin/sh
# Host unit tests for the netif configuration and link-event state machine
# (kernel/net/lwip_stack.rs): the carrier detector, the deferred flush, the
# address/MTU/flag transactions and the transmit entry.  The primitives are
# stubbed inside the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/lwip_stack.rs -o "$tmpdir/lwip-stack-tests"
"$tmpdir/lwip-stack-tests"
