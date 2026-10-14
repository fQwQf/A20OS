#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
: "${BUILD_DIR:?set BUILD_DIR to an isolated output directory}"
build_dir=$(mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR" && pwd)
target="$repo/tools/rust-targets/riscv32imafdc-a20.json"
prefix=${RISCV_ELF_PREFIX:-riscv64-unknown-elf-}
cc=${CC:-${prefix}gcc}
ld=${LD:-${prefix}ld}
nm=${NM:-${prefix}nm}
rustc=${RUSTC:-rustc}
flags=(ARCH=riscv32 ABI=both NOMMU=0 NR_CPUS=1 BUILD_DIR="$build_dir"
       PARK_WAIT_IMPL=rust CHANNEL_RECV_TXN_IMPL=rust WAIT_TIMER_IMPL=rust
       WAIT_QUEUE_IMPL=rust RW_MUTEX_IMPL=rust MUTEX_STATE_IMPL=rust
       COMPLETION_IMPL=rust)

make -C "$repo" "${flags[@]}" -j2 \
    "$build_dir/rust/wait_timer.o" \
    "$build_dir/rust/wait_queue_state.o" \
    "$build_dir/rust/rw_mutex_state.o" \
    "$build_dir/rust/mutex_state.o" \
    "$build_dir/rust/completion_state.o"

generated="$build_dir/generated"
"$cc" -c -Wall -Wextra -Werror -ffreestanding -fno-builtin \
    -march=rv32imafdc -mabi=ilp32d -DRISCV32 -DCONFIG_RISCV32 \
    -DCONFIG_ABI_BOTH -DCONFIG_NR_CPUS=1 \
    -I"$repo/kernel/arch/riscv32/include" -I"$repo/kernel/include" \
    -I"$repo/kernel" -I"$generated" \
    "$repo/tools/tests/rust_rv32_sync_abi.c" \
    -o "$build_dir/rust/rv32_sync_abi_c.o"

RUSTC_BOOTSTRAP=1 "$rustc" --edition=2021 --crate-type lib --emit=obj \
    -Z unstable-options --sysroot "$build_dir/rust/rv32-sysroot" \
    --target "$target" -C panic=abort -C opt-level=2 \
    -C codegen-units=1 -C relocation-model=static \
    "$repo/tools/tests/rust_rv32_sync_abi.rs" \
    -o "$build_dir/rust/rv32_sync_abi_rust.o"

"$ld" -m elf32lriscv -r \
    "$build_dir/rust/rv32_sync_abi_c.o" \
    "$build_dir/rust/rv32_sync_abi_rust.o" \
    -o "$build_dir/rust/rv32_sync_abi_combined.o"
"$nm" "$build_dir/rust/rv32_sync_abi_combined.o" | \
    grep -q 'a20_rv32_sync_c_layout_probe'
"$nm" "$build_dir/rust/rv32_sync_abi_combined.o" | \
    grep -q 'a20_rv32_sync_rust_layout_probe'
file "$build_dir/rust/rv32_sync_abi_combined.o"
echo 'RV32 sync ABI C/Rust layout probe: PASS'
