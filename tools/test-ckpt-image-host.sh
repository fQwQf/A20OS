#!/bin/sh
# Host unit tests for kernel/proc/ckpt_image.rs: the A20CKPT1 validation
# logic and the restore-frame assembly whose ownership model (rv64 slot x[0]
# is the address-space-token scratch, not a GPR) exists to make the
# token-clobber ordering bug unrepresentable.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/a20-ckpt-image-tests
mkdir -p "$out"
cd "$root"
rustc --edition=2021 --test kernel/proc/ckpt_image.rs \
    -o "$out/ckpt-image-tests" > "$out/rust-test.log" 2>&1 || {
    cat "$out/rust-test.log"
    exit 1
}
"$out/ckpt-image-tests"
