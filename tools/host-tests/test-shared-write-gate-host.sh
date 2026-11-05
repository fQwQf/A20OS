#!/bin/sh
# Host tests for the shared-write admission gate (kernel/mm/shutdown_write.rs).
#
# The gate's decisions are tested by the module's own #[cfg(test)] suite, which
# is the only place the threaded races can be driven: the commit-vs-abort race
# (exactly one of the two may win), the drain race (a transaction that entered
# before the close must be observed as still in flight until it leaves), the
# generation-exhaustion path, and the Busy contention path that a
# single-threaded C harness cannot reach because the control word is private.
#
# The module is compiled for the host with --test (not with the kernel's
# -C panic=abort, which the test harness does not support); the production
# entry points and the state machine under test are the same code the kernel
# links.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-shared-write-gate.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
rustc --test --edition=2021 "$repo/kernel/mm/shutdown_write.rs" -o "$tmp/gate-tests"
# Serialise the harness's own scheduling so a failure names one test; the race
# tests spawn their own threads and synchronise with barriers, so this does not
# weaken them.
"$tmp/gate-tests" --test-threads=1
