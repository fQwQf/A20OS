#!/bin/sh
# Block cache state engine: Rust unit tests + C/Rust differential harness.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${EVIDENCE_DIR:?set EVIDENCE_DIR to a persistent, private output directory}
mkdir -p "$out"
cd "$root"
sha256sum kernel/fs/block_cache_state.rs kernel/include/fs/block_cache.h \
    tools/tests/block_cache_state_host.c tools/test-block-cache-state-host.sh \
    > "$out/source.sha256"

rustc --edition=2021 --test kernel/fs/block_cache_state.rs \
    -o "$out/block-cache-state-tests" > "$out/rust-test.log" 2>&1
"$out/block-cache-state-tests" >> "$out/rust-test.log" 2>&1

# Host staticlib: the module is a crate root in production, so strip the doc
# block and the crate-level attribute and supply a host panic handler.
{
    printf '#![no_std]\n#![allow(dead_code)]\n'
    printf 'extern "C" { fn abort() -> !; }\n'
    printf '#[panic_handler]\n'
    printf 'fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }\n'
    sed -e '1,/^#!\[cfg_attr(not(test), no_std)\]$/d' kernel/fs/block_cache_state.rs
} > "$out/block-cache-state-host-lib.rs"
rustc --edition=2021 --crate-name a20_block_cache_state --crate-type staticlib \
    -C panic=abort -C opt-level=3 "$out/block-cache-state-host-lib.rs" \
    -o "$out/liba20_block_cache_state.a"
printf 'void rust_eh_personality(void) {}\n' > "$out/host-stub.c"

rustc --edition=2021 --crate-name a20_block_cache_state -C panic=abort \
    --crate-type lib --emit=obj --target riscv32imafc-unknown-none-elf \
    kernel/fs/block_cache_state.rs -o "$out/block-cache-state-rv32.o"

cc -std=gnu11 -O2 -Wall -Wextra -Werror \
    tools/tests/block_cache_state_host.c "$out/host-stub.c" \
    "$out/liba20_block_cache_state.a" -o "$out/block-cache-state-c-rust"
"$out/block-cache-state-c-rust" > "$out/differential.log" 2>&1 || {
    cat "$out/differential.log" >&2
    exit 1
}
cat "$out/differential.log"

sha256sum "$out/block-cache-state-tests" "$out/liba20_block_cache_state.a" \
    "$out/block-cache-state-rv32.o" "$out/block-cache-state-c-rust" \
    > "$out/output.sha256"
echo 'block cache state Rust host tests + C->Rust differential PASS'
