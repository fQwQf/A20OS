#!/bin/sh
# Page cache state engine: Rust unit tests + C/Rust differential harness.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${EVIDENCE_DIR:?set EVIDENCE_DIR to a persistent, private output directory}
RUST_RV32_TARGET=${RUST_RV32_TARGET:-$root/tools/riscv32imafdc-a20.json}
mkdir -p "$out"
cd "$root"
sha256sum kernel/fs/page_cache_state.rs kernel/include/fs/page_cache.h \
    tools/tests/page_cache_state_host.c tools/test-page-cache-state-host.sh \
    > "$out/source.sha256"

rustc --edition=2021 --test kernel/fs/page_cache_state.rs \
    -o "$out/page-cache-state-tests" > "$out/rust-test.log" 2>&1
"$out/page-cache-state-tests" >> "$out/rust-test.log" 2>&1

# Host staticlib: the module is a crate root in production, so strip the doc
# block and the crate-level attribute and supply a host panic handler.
{
    printf '#![no_std]\n#![allow(dead_code)]\n'
    printf 'extern "C" { fn abort() -> !; }\n'
    printf '#[panic_handler]\n'
    printf 'fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }\n'
    sed -e '1,/^#!\[cfg_attr(not(test), no_std)\]$/d' kernel/fs/page_cache_state.rs
} > "$out/page-cache-state-host-lib.rs"
rustc --edition=2021 --crate-name a20_page_cache_state --crate-type staticlib \
    -C panic=abort -C opt-level=3 "$out/page-cache-state-host-lib.rs" \
    -o "$out/liba20_page_cache_state.a"
printf 'void rust_eh_personality(void) {}\n' > "$out/host-stub.c"

for target in riscv64imac-unknown-none-elf riscv32imafc-unknown-none-elf \
              x86_64-unknown-none aarch64-unknown-none; do
    case "$target" in
        riscv32*) [ -n "${RUST_RV32_TARGET:-}" ] || continue ;;
    esac
    rustc --edition=2021 --crate-name a20_page_cache_state -C panic=abort \
        --crate-type lib --emit=obj --target "$target" \
        kernel/fs/page_cache_state.rs -o "$out/page-cache-state-$target.o"
done

cc -std=gnu11 -O2 -Wall -Wextra -Werror \
    tools/tests/page_cache_state_host.c "$out/host-stub.c" \
    "$out/liba20_page_cache_state.a" -o "$out/page-cache-state-c-rust"
"$out/page-cache-state-c-rust" 200000 > "$out/differential.log" 2>&1 || {
    cat "$out/differential.log" >&2
    exit 1
}
cat "$out/differential.log"

sha256sum "$out/page-cache-state-tests" "$out/liba20_page_cache_state.a" \
    "$out/page-cache-state-c-rust" > "$out/output.sha256"
echo 'page cache state Rust host tests + C->Rust differential PASS'
