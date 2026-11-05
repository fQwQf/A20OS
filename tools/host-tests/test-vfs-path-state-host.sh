#!/bin/sh
# VFS path/name state engine: Rust unit tests + C/Rust differential harness.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${EVIDENCE_DIR:?set EVIDENCE_DIR to a persistent, private output directory}
RUST_RV32_TARGET=${RUST_RV32_TARGET:-$root/tools/riscv32imafdc-a20.json}
mkdir -p "$out"
cd "$root"
sha256sum kernel/fs/vfs/path_state.rs kernel/include/fs/vfs/path_state.h \
    tools/tests/path_state_host.c tools/host-tests/test-vfs-path-state-host.sh \
    > "$out/source.sha256"

rustc --edition=2021 --test kernel/fs/vfs/path_state.rs \
    -o "$out/path-state-tests" > "$out/rust-test.log" 2>&1
"$out/path-state-tests" >> "$out/rust-test.log" 2>&1

{
    printf '#![no_std]\n#![allow(dead_code)]\n'
    printf 'extern "C" { fn abort() -> !; }\n'
    printf '#[panic_handler]\n'
    printf 'fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }\n'
    sed -e '1,/^#!\[cfg_attr(not(test), no_std)\]$/d' kernel/fs/vfs/path_state.rs
} > "$out/path-state-host-lib.rs"
rustc --edition=2021 --crate-name a20_vfs_path_state --crate-type staticlib \
    -C panic=abort -C opt-level=3 "$out/path-state-host-lib.rs" \
    -o "$out/liba20_vfs_path_state.a"
printf 'void rust_eh_personality(void) {}\n' > "$out/host-stub.c"

for target in riscv64imac-unknown-none-elf riscv32imafc-unknown-none-elf \
              x86_64-unknown-none aarch64-unknown-none; do
    case "$target" in
        riscv32*) [ -n "${RUST_RV32_TARGET:-}" ] || continue ;;
    esac
    rustc --edition=2021 --crate-name a20_vfs_path_state -C panic=abort \
        --crate-type lib --emit=obj --target "$target" \
        kernel/fs/vfs/path_state.rs -o "$out/path-state-$target.o"
done

cc -std=gnu11 -O2 -Wall -Wextra -Werror \
    -Wno-format-truncation -Wno-stringop-truncation \
    tools/tests/path_state_host.c "$out/host-stub.c" \
    "$out/liba20_vfs_path_state.a" -o "$out/path-state-c-rust"
"$out/path-state-c-rust" 200000 > "$out/differential.log" 2>&1 || {
    cat "$out/differential.log" >&2
    exit 1
}
cat "$out/differential.log"

sha256sum "$out/path-state-tests" "$out/liba20_vfs_path_state.a" \
    "$out/path-state-c-rust" > "$out/output.sha256"
echo 'vfs path state Rust host tests + C->Rust differential PASS'
