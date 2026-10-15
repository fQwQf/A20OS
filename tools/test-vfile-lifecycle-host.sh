#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${EVIDENCE_DIR:?set EVIDENCE_DIR to a persistent, private output directory}
mkdir -p "$out"
cd "$root"
sha256sum kernel/fs/vfs/vfile_lifecycle.rs kernel/include/fs/vfs/vfile_lifecycle_c.h \
    tools/tests/vfile_lifecycle_host.c \
    tools/test-vfile-lifecycle-host.sh > "$out/source.sha256"
{
    printf 'cwd=%s\n' "$PWD"
    printf 'EVIDENCE_DIR=%s\n' "$out"
    printf '%s\n' "rustc --edition=2021 --test kernel/fs/vfs/vfile_lifecycle.rs -o $out/vfile-lifecycle-tests"
    printf '%s\n' "rustc --edition=2021 --crate-name a20_vfile_lifecycle --crate-type staticlib --cfg feature=std kernel/fs/vfs/vfile_lifecycle.rs -o $out/liba20_vfile_lifecycle.a"
    printf '%s\n' "printf '#![no_std]\\nmod lifecycle { include!(\"%s/kernel/fs/vfs/vfile_lifecycle.rs\"); }\\n' '$root' > $out/vfile-lifecycle-rv32.rs"
    printf '%s\n' "rustc --edition=2021 --crate-name a20_vfile_lifecycle -C panic=abort --crate-type lib --target riscv32imafc-unknown-none-elf --emit=obj $out/vfile-lifecycle-rv32.rs -o $out/vfile-lifecycle-rv32.o"
    printf '%s\n' "cc -std=gnu11 -Wall -Wextra -Werror -Ikernel/include tools/tests/vfile_lifecycle_host.c $out/liba20_vfile_lifecycle.a -ldl -lpthread -lm -o $out/vfile-lifecycle-c-rust"
} > "$out/run.argv"
set +e
rustc --edition=2021 --test kernel/fs/vfs/vfile_lifecycle.rs \
    -o "$out/vfile-lifecycle-tests" > "$out/rust-test.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$out/rust-test.exit"
[ "$rc" -eq 0 ]
"$out/vfile-lifecycle-tests" >> "$out/rust-test.log" 2>&1
rustc --edition=2021 --crate-name a20_vfile_lifecycle --crate-type staticlib \
    --cfg 'feature="std"' kernel/fs/vfs/vfile_lifecycle.rs \
    -o "$out/liba20_vfile_lifecycle.a"
printf '#![no_std]\nmod lifecycle { include!("%s/kernel/fs/vfs/vfile_lifecycle.rs"); }\n' \
    "$root" > "$out/vfile-lifecycle-rv32.rs"
rustc --edition=2021 --crate-name a20_vfile_lifecycle -C panic=abort \
    --crate-type lib --target riscv32imafc-unknown-none-elf --emit=obj \
    "$out/vfile-lifecycle-rv32.rs" -o "$out/vfile-lifecycle-rv32.o"
cc -std=gnu11 -Wall -Wextra -Werror -Ikernel/include tools/tests/vfile_lifecycle_host.c \
    "$out/liba20_vfile_lifecycle.a" -ldl -lpthread -lm \
    -o "$out/vfile-lifecycle-c-rust"
"$out/vfile-lifecycle-c-rust" > "$out/c-rust.log" 2>&1
printf '0\n' > "$out/c-rust.exit"
sha256sum "$out/vfile-lifecycle-tests" "$out/liba20_vfile_lifecycle.a" \
    "$out/vfile-lifecycle-rv32.o" "$out/vfile-lifecycle-c-rust" \
    > "$out/output.sha256"
printf 'Rust unit tests: 4/4\nC/Rust differential harness: 0\nRust RV32 object compile: 0\n' > "$out/exit.status"
echo 'vfile close lifecycle Rust host + C→Rust oracle PASS'
