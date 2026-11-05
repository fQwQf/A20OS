#!/bin/sh
# Paired C/Rust microbenchmark for the canonical vnode identity index.
#
# Fixed protocol: one CPU (taskset), -O3 for both models, one shared machine
# and one process so both run back-to-back; the driver alternates the C->Rust /
# Rust->C round order and reports medians.  Run under the shared activity EX
# lock so no concurrent build perturbs the measurement.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${EVIDENCE_DIR:?set EVIDENCE_DIR to a persistent, private output directory}
cpus=${BENCH_CPUS:-0}
rounds=${BENCH_ROUNDS:-31}
mkdir -p "$out"
cd "$root"

sha256sum kernel/fs/vfs/vnode_identity.rs kernel/include/fs/vfs/vnode_identity.h \
    kernel/include/fs/vfs/vnode_identity_c.h tools/tests/vnode_identity_bench.c \
    tools/host-tests/test-uxfs-vnode-identity-bench.sh > "$out/source.sha256"

# Host staticlib wrapper (same transform as the differential harness): the
# module file is a crate root in production, so strip its crate-level doc
# block and no_std attribute and supply a host panic handler.
{
    printf '#![no_std]\n#![allow(dead_code)]\n'
    printf 'extern "C" { fn abort() -> !; }\n'
    printf '#[panic_handler]\n'
    printf 'fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }\n'
    sed -e '1,/^#!\[cfg_attr(not(test), no_std)\]$/d' \
        kernel/fs/vfs/vnode_identity.rs
} > "$out/vnode-identity-host-lib.rs"
rustc --edition=2021 --crate-name a20_vnode_identity --crate-type staticlib \
    -C panic=abort -C opt-level=3 "$out/vnode-identity-host-lib.rs" \
    -o "$out/liba20_vnode_identity.a"
printf 'void rust_eh_personality(void) {}\n' > "$out/vnode-identity-host-stub.c"
cc -std=gnu11 -O3 -Wall -Wextra -Werror -Ikernel/include \
    tools/tests/vnode_identity_bench.c "$out/vnode-identity-host-stub.c" \
    "$out/liba20_vnode_identity.a" -o "$out/vnode-identity-bench"

{
    printf 'workload=fill,dup-insert,lookup,remove,prune cap=16384 rounds=%s\n' \
        "$rounds"
    printf '%s\n' "taskset -c $cpus $out/vnode-identity-bench"
} > "$out/run.argv"

# Round count is compiled in; keep BENCH_ROUNDS advisory to the driver.
BENCH_ROUNDS_VALUE=$rounds
if [ "$BENCH_ROUNDS_VALUE" != "31" ]; then
    echo "note: driver round count is compiled-in (31); BENCH_ROUNDS=$rounds ignored" \
        >&2
fi

taskset -c "$cpus" "$out/vnode-identity-bench" | tee "$out/bench.csv"
sha256sum "$out/vnode-identity-bench" > "$out/output.sha256"
echo 'vnode identity paired host benchmark PASS'
