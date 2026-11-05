#!/bin/sh
# UXFS lifetime negotiation: Rust unit tests + C host ABI harness.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${EVIDENCE_DIR:?set EVIDENCE_DIR to a persistent, private output directory}
mkdir -p "$out"
cd "$root"
sha256sum kernel/fs/uxfs/lifetime_state.rs kernel/include/fs/uxfs_lifetime_state.h \
    tools/tests/uxfs_lifetime_state_host.c tools/host-tests/test-uxfs-lifetime-state-host.sh \
    > "$out/source.sha256"

rustc --edition=2021 --test kernel/fs/uxfs/lifetime_state.rs \
    -o "$out/uxfs-lifetime-state-tests" > "$out/rust-test.log" 2>&1
"$out/uxfs-lifetime-state-tests" >> "$out/rust-test.log" 2>&1

{
    printf '#![no_std]\n#![allow(dead_code)]\n'
    printf 'extern "C" { fn abort() -> !; }\n'
    printf '#[panic_handler]\n'
    printf 'fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }\n'
    sed -e '1,/^#!\[cfg_attr(not(test), no_std)\]$/d' kernel/fs/uxfs/lifetime_state.rs
} > "$out/uxfs-lifetime-state-host-lib.rs"
rustc --edition=2021 --crate-name a20_uxfs_lifetime_state --crate-type staticlib \
    -C panic=abort -C opt-level=3 "$out/uxfs-lifetime-state-host-lib.rs" \
    -o "$out/liba20_uxfs_lifetime_state.a"
printf 'void rust_eh_personality(void) {}\n' > "$out/host-stub.c"

for target in riscv64imac-unknown-none-elf x86_64-unknown-none aarch64-unknown-none; do
    rustc --edition=2021 --crate-name a20_uxfs_lifetime_state -C panic=abort \
        --crate-type lib --emit=obj --target "$target" \
        kernel/fs/uxfs/lifetime_state.rs -o "$out/uxfs-lifetime-state-$target.o"
done

cc -std=gnu11 -O2 -Wall -Wextra -Werror \
    tools/tests/uxfs_lifetime_state_host.c "$out/host-stub.c" \
    "$out/liba20_uxfs_lifetime_state.a" -o "$out/uxfs-lifetime-state-host"
"$out/uxfs-lifetime-state-host" > "$out/host.log" 2>&1 || {
    cat "$out/host.log" >&2
    exit 1
}
cat "$out/host.log"

sha256sum "$out/uxfs-lifetime-state-tests" "$out/liba20_uxfs_lifetime_state.a" \
    "$out/uxfs-lifetime-state-host" > "$out/output.sha256"
echo 'UXFS lifetime state Rust unit tests + C host ABI harness PASS'
