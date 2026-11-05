#!/bin/sh
# Open-file I/O-path policy engine: Rust unit tests + target build smoke.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${EVIDENCE_DIR:?set EVIDENCE_DIR to a persistent, private output directory}
RUST_RV32_TARGET=${RUST_RV32_TARGET:-$root/tools/riscv32imafdc-a20.json}
mkdir -p "$out"
cd "$root"
sha256sum kernel/fs/vfs/file_state.rs kernel/include/fs/vfs/file_state.h \
    tools/host-tests/test-file-state-host.sh > "$out/source.sha256"

rustc --edition=2021 --test kernel/fs/vfs/file_state.rs \
    -o "$out/file-state-tests" > "$out/rust-test.log" 2>&1
"$out/file-state-tests" >> "$out/rust-test.log" 2>&1 || {
    cat "$out/rust-test.log" >&2
    exit 1
}

{
    printf '#![no_std]\n#![allow(dead_code)]\n'
    printf 'extern "C" { fn abort() -> !; }\n'
    printf '#[panic_handler]\n'
    printf 'fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }\n'
    sed -e '1,/^#!\[cfg_attr(not(test), no_std)\]$/d' kernel/fs/vfs/file_state.rs
} > "$out/file-state-host-lib.rs"
rustc --edition=2021 --crate-name a20_vfs_file_state --crate-type staticlib \
    -C panic=abort -C opt-level=3 "$out/file-state-host-lib.rs" \
    -o "$out/liba20_vfs_file_state.a"

for target in riscv64imac-unknown-none-elf riscv32imafc-unknown-none-elf \
              x86_64-unknown-none aarch64-unknown-none; do
    case "$target" in
        riscv32*) [ -n "${RUST_RV32_TARGET:-}" ] || continue ;;
    esac
    rustc --edition=2021 --crate-name a20_vfs_file_state -C panic=abort \
        --crate-type lib --emit=obj --target "$target" \
        kernel/fs/vfs/file_state.rs -o "$out/file-state-$target.o"
done

sha256sum "$out/file-state-tests" "$out/liba20_vfs_file_state.a" \
    > "$out/output.sha256"
echo 'file state Rust host tests + target build smoke PASS'
