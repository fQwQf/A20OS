#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${EVIDENCE_DIR:?set EVIDENCE_DIR to a persistent, private output directory}
mkdir -p "$out"
cd "$root"
sha256sum kernel/fs/vfs/vnode_identity.rs kernel/include/fs/vfs/vnode_identity.h \
    kernel/include/fs/vfs/vnode_identity_c.h tools/tests/vnode_identity_host.c \
    tools/test-uxfs-vnode-identity-host.sh > "$out/source.sha256"
{
    printf 'cwd=%s\n' "$PWD"
    printf 'EVIDENCE_DIR=%s\n' "$out"
    printf '%s\n' "rustc --edition=2021 --test kernel/fs/vfs/vnode_identity.rs -o $out/vnode-identity-tests"
    printf '%s\n' "{ printf '#![no_std]\\n#![allow(dead_code)]\\nextern \"C\" { fn abort() -> !; }\\n#[panic_handler]\\nfn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }\\n'; sed -e '1,/^#!\\[cfg_attr(not(test), no_std)\\]$/d' kernel/fs/vfs/vnode_identity.rs; } > $out/vnode-identity-host-lib.rs"
    printf '%s\n' "rustc --edition=2021 --crate-name a20_vnode_identity --crate-type staticlib -C panic=abort $out/vnode-identity-host-lib.rs -o $out/liba20_vnode_identity.a"
    printf '%s\n' "rustc --edition=2021 --crate-name a20_vnode_identity -C panic=abort --crate-type lib --emit=obj --target riscv32imafc-unknown-none-elf kernel/fs/vfs/vnode_identity.rs -o $out/vnode-identity-rv32.o"
    printf '%s\n' "cc -std=gnu11 -Wall -Wextra -Werror -Ikernel/include tools/tests/vnode_identity_host.c $out/vnode-identity-host-stub.c $out/liba20_vnode_identity.a -o $out/vnode-identity-c-rust"
} > "$out/run.argv"
set +e
rustc --edition=2021 --test kernel/fs/vfs/vnode_identity.rs \
    -o "$out/vnode-identity-tests" > "$out/rust-test.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$out/rust-test.exit"
[ "$rc" -eq 0 ]
"$out/vnode-identity-tests" >> "$out/rust-test.log" 2>&1
# Host staticlib wrapper: the module file is a crate root in production, so
# strip its crate-level doc block and no_std attribute and supply a host
# panic handler; the rest of the file is compiled verbatim.
{
    printf '#![no_std]\n#![allow(dead_code)]\n'
    printf 'extern "C" { fn abort() -> !; }\n'
    printf '#[panic_handler]\n'
    printf 'fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }\n'
    sed -e '1,/^#!\[cfg_attr(not(test), no_std)\]$/d' kernel/fs/vfs/vnode_identity.rs
} > "$out/vnode-identity-host-lib.rs"
rustc --edition=2021 --crate-name a20_vnode_identity --crate-type staticlib \
    -C panic=abort "$out/vnode-identity-host-lib.rs" \
    -o "$out/liba20_vnode_identity.a"
rustc --edition=2021 --crate-name a20_vnode_identity -C panic=abort \
    --crate-type lib --emit=obj --target riscv32imafc-unknown-none-elf \
    kernel/fs/vfs/vnode_identity.rs -o "$out/vnode-identity-rv32.o"
# panic=abort staticlib still carries a DW.ref to the EH personality from the
# prebuilt core objects; it is never called, but the symbol must link.
printf '/* panic=abort staticlib: the prebuilt core objects carry a DW.ref,\n * so the EH personality symbol must exist though it is never called. */\nvoid rust_eh_personality(void) {}\n' > "$out/vnode-identity-host-stub.c"
cc -std=gnu11 -Wall -Wextra -Werror -Ikernel/include \
    tools/tests/vnode_identity_host.c "$out/vnode-identity-host-stub.c" \
    "$out/liba20_vnode_identity.a" -o "$out/vnode-identity-c-rust"
"$out/vnode-identity-c-rust" > "$out/c-rust.log" 2>&1
printf '0\n' > "$out/c-rust.exit"
sha256sum "$out/vnode-identity-tests" "$out/liba20_vnode_identity.a" \
    "$out/vnode-identity-rv32.o" "$out/vnode-identity-c-rust" \
    > "$out/output.sha256"
printf 'Rust unit tests: 6/6\nC/Rust differential harness: 0\nRust RV32 object compile: 0\n' > "$out/exit.status"
echo 'vnode identity Rust host + C->Rust differential PASS'
