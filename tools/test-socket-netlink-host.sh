#!/bin/sh
# Host unit tests for the AF_NETLINK state machines (kernel/net/socket_netlink.rs):
# bind, sock_diag, the uevent emitter/sender and the rtnetlink request/response
# and multicast paths.  The primitives are stubbed inside the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket_netlink.rs -o "$tmpdir/socket-netlink-tests"
"$tmpdir/socket-netlink-tests"
