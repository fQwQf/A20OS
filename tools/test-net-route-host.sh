#!/bin/sh
# The production Rust FIB with normal and embedded resource budgets.
set -eu
cd "$(dirname "$0")/.."
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rustc --edition=2021 --test kernel/net/control/route.rs -o "$tmpdir/routes"
"$tmpdir/routes"
rustc --edition=2021 --test --cfg a20_net_embedded kernel/net/control/route.rs -o "$tmpdir/routes-embedded"
"$tmpdir/routes-embedded"
