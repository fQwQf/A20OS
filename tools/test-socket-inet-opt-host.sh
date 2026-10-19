#!/bin/sh
# Host unit tests for the INET option and buffer machinery
# (kernel/net/socket_inet_opt.rs): the congestion switch, the ceilings and the
# value in force, the buffer application, the option application and the IP
# option snapshot/reapply.  The primitives are stubbed inside the test module.
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/socket_inet_opt.rs -o "$tmpdir/socket-inet-opt-tests"
"$tmpdir/socket-inet-opt-tests"
