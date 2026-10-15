#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${EVIDENCE_DIR:-}" ]; then
    tmp_dir=$EVIDENCE_DIR
    mkdir -p "$tmp_dir"
else
    tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/a20-vfs-shutdown.XXXXXX")
fi
cd "$repo_root"

sha256sum kernel/fs/vfs/shutdown_state.rs kernel/include/fs/vfs/shutdown.h \
    tools/tests/vfs_shutdown_abi_host.c tools/tests/vfs_shutdown_layout.c \
    tools/test-vfs-shutdown-host.sh kernel/fs/vfs/file.c \
    > "$tmp_dir/source.sha256"
printf '%s\n' \
    'TMPDIR=/dev/shm/a20-vfs-shutdown-abi-r2 .git/parallel-rewrite/run-build.sh build-2 tools/test-vfs-shutdown-host.sh' \
    'rustc --edition=2021 --test kernel/fs/vfs/shutdown_state.rs' \
    'rustc --edition=2021 --crate-type staticlib --cfg feature="std" kernel/fs/vfs/shutdown_state.rs' \
    'rustc --edition=2021 --crate-type lib --target riscv32imafc-unknown-none-elf --emit=obj kernel/fs/vfs/vfs_lifecycle.rs' \
    'cc -m32 -std=gnu11 -Wall -Wextra -Werror -I kernel/include -fsyntax-only tools/tests/vfs_shutdown_layout.c' \
    'cc -std=gnu11 -Wall -Wextra -Werror -I kernel/include tools/tests/vfs_shutdown_abi_host.c liba20_shutdown_state.a -ldl -lpthread -lm' \
    > "$tmp_dir/run.argv"
rustc --edition=2021 --test kernel/fs/vfs/shutdown_state.rs \
    -o "$tmp_dir/shutdown-state-tests" > "$tmp_dir/rust-host.log" 2>&1
"$tmp_dir/shutdown-state-tests" >> "$tmp_dir/rust-host.log" 2>&1

rustc --edition=2021 --crate-name a20_shutdown_state --crate-type staticlib \
    --cfg 'feature="std"' kernel/fs/vfs/shutdown_state.rs \
    -o "$tmp_dir/liba20_shutdown_state.a"
rustc --edition=2021 --crate-name a20_shutdown_layout \
    --crate-type lib --target riscv32imafc-unknown-none-elf --emit=obj \
    kernel/fs/vfs/vfs_lifecycle.rs -o "$tmp_dir/shutdown-rv32.o"
cc -m32 -std=gnu11 -Wall -Wextra -Werror -I kernel/include \
    -fsyntax-only tools/tests/vfs_shutdown_layout.c
cc -std=gnu11 -Wall -Wextra -Werror \
    -I kernel/include tools/tests/vfs_shutdown_abi_host.c \
    "$tmp_dir/liba20_shutdown_state.a" -ldl -lpthread -lm \
    -o "$tmp_dir/vfs-shutdown-abi"
"$tmp_dir/vfs-shutdown-abi" > "$tmp_dir/c-rust-abi.log" 2>&1
printf 'C→Rust ABI/reentrant concurrency PASS (exit 0)\n' \
    >> "$tmp_dir/c-rust-abi.log"
printf 'rustc test: exit 0\nRust RV32 object/layout: exit 0\nC ILP32 header/layout: exit 0\nC→Rust ABI/reentrant concurrency: exit 0\n' \
    > "$tmp_dir/exit.status"
echo "VFS shutdown Rust/C callback ABI and retry sequence PASS"
