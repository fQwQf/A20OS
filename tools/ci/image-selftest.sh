#!/bin/sh
# 镜像自检：这个 target 声称的工具链是否真的在里面。
#
# 刻意不跑 check-<arch>-bringup。那条门禁回答的是"这棵树能不能编"，是源码
# 问题。把两者混在一起，镜像缺件会被误读成源码坏了，源码坏了
# 又会让镜像看起来缺件。这里只证明三件事：工具链在、能为目标产生对象、Rust
# 侧该有的 target 与组件齐全。
#
# 用法（由 tools/ci/build-image.sh 调用，也可单独跑）：
#   docker run --rm -v "$PWD:/src" -w /src a20os-buildenv:riscv \
#       sh tools/ci/image-selftest.sh riscv
set -eu

target=${1:?usage: image-selftest.sh <target>}
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { echo "image-selftest: FAIL ($target): $*" >&2; exit 1; }
note() { echo "image-selftest: $target: $*"; }

printf 'int probe(void){return 0;}\n' > "$tmp/probe.c"
printf '#![no_std]\npub fn probe() -> u32 { 0 }\n' > "$tmp/probe.rs"
printf '#include <limits.h>\n#include <stdint.h>\n' > "$tmp/libc.c"

common() {
    for tool in make python3 ccache mtools mcopy mkfs.ext4 rg clang-19; do
        command -v "$tool" >/dev/null 2>&1 || fail "$tool missing"
    done
    command -v rustc >/dev/null 2>&1 || fail "rustc missing"
    command -v cargo >/dev/null 2>&1 || fail "cargo missing"
    note "rust: $(rustc --version)"
    cc_object bsd-frontend clang-19
}

cc_object() { # cc_object <label> <cc> [flags...]
    label=$1
    shift
    command -v "$1" >/dev/null 2>&1 || fail "$1 missing"
    cc=$1
    shift
    "$cc" -ffreestanding -nostdlib -c "$@" "$tmp/probe.c" -o "$tmp/$label.o" \
        || fail "$cc cannot compile a freestanding object"
    note "cc: $label ($cc $*) ok"
}

# 交叉 gcc 只把 libc6-dev-*-cross 列为 Recommends，而构建用
# --no-install-recommends：于是编译器在、头文件不在。内核里确实有包含 libc
# 头的文件（kernel/ipc/keyring.c 的 <limits.h> 就是 arm32 曾经报的那个错），
# 所以这一项要显式检查，不能等某个架构碰巧用到才发现。
cc_libc_headers() { # cc_libc_headers <cc>
    command -v "$1" >/dev/null 2>&1 || fail "$1 missing"
    "$1" -E "$tmp/libc.c" -o /dev/null \
        || fail "$1 cannot preprocess libc headers (install libc6-dev-*-cross)"
    note "cc: $1 libc headers ok"
}

rust_target() { # rust_target <triple>
    command -v rustc >/dev/null 2>&1 || fail "rustc missing"
    rustc --target "$1" --crate-type lib --emit=metadata \
          -o "$tmp/$1.rmeta" "$tmp/probe.rs" \
        || fail "rustc has no core for $1 (missing rustup target)"
    note "rust: $1 ok"
}

# riscv32 是自定义 JSON 目标，没有预编译 core：构建入口用
# `-Z build-std=core` 从 rust-src 组件现场编 core。只有真的编一次才能证明
# rust-src 可用——缺了它，riscv32 的第一个 Rust 对象就会失败。
rust_custom_rv32() {
    spec=$repo/tools/rust/targets/riscv32imafdc-a20.json
    [ -f "$spec" ] || fail "missing $spec"
    mkdir -p "$tmp/rv32/src"
    printf '[package]\nname="selftest"\nversion="0.0.0"\nedition="2021"\n[lib]\npath="src/lib.rs"\n' \
        > "$tmp/rv32/Cargo.toml"
    printf '#![no_std]\npub fn probe() -> u32 { 0 }\n' > "$tmp/rv32/src/lib.rs"
    ( cd "$tmp/rv32" \
      && RUSTC_BOOTSTRAP=1 CARGO_TARGET_DIR="$tmp/rv32/out" \
         cargo build -q -Z json-target-spec -Z build-std=core --release \
         --target "$spec" ) \
        || fail "cannot build core for the riscv32 custom target (rust-src component)"
    note "rust: riscv32imafdc-a20 (-Z build-std=core) ok"
}

qemu() { # qemu <binary>
    command -v "$1" >/dev/null 2>&1 || fail "$1 missing"
    "$1" --version > /dev/null || fail "$1 does not run"
    note "qemu: $1 ok"
}

arch_riscv() {
    cc_object rv64-elf riscv64-unknown-elf-gcc -march=rv64gc -mabi=lp64d
    cc_object rv32-elf riscv64-unknown-elf-gcc -march=rv32imafdc -mabi=ilp32d
    cc_object rv64-linux riscv64-linux-gnu-gcc
    cc_libc_headers riscv64-linux-gnu-gcc
    rust_target riscv64imac-unknown-none-elf
    rust_target riscv64gc-unknown-none-elf
    rust_custom_rv32
    qemu qemu-system-riscv64
    qemu qemu-system-riscv32
}

arch_aarch64() {
    cc_object aarch64 aarch64-linux-gnu-gcc
    cc_libc_headers aarch64-linux-gnu-gcc
    rust_target aarch64-unknown-none
    qemu qemu-system-aarch64
}

arch_x86_64() {
    cc_object x86_64 cc
    rust_target x86_64-unknown-none
    qemu qemu-system-x86_64
}

arch_loongarch64() {
    cc_object loongarch64 loongarch64-linux-gnu-gcc
    cc_libc_headers loongarch64-linux-gnu-gcc
    rust_target loongarch64-unknown-none
    qemu qemu-system-loongarch64
}

arch_arm32() {
    cc_object arm32 arm-linux-gnueabihf-gcc -march=armv7-a -marm \
        -mfpu=vfpv3-d16 -mfloat-abi=hard
    cc_libc_headers arm-linux-gnueabihf-gcc
    rust_target armv7a-none-eabihf
    qemu qemu-system-arm
}

arch_armv7m() {
    cc_object armv7m arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb
    rust_target thumbv7m-none-eabi
    qemu qemu-system-arm
}

common
case "$target" in
base) ;;
riscv) arch_riscv ;;
aarch64) arch_aarch64 ;;
x86_64) arch_x86_64 ;;
loongarch64) arch_loongarch64 ;;
arm32) arch_arm32 ;;
armv7m) arch_armv7m ;;
multi)
    arch_riscv
    arch_aarch64
    arch_x86_64
    arch_loongarch64
    arch_arm32
    arch_armv7m
    ;;
*) fail "unknown target '$target'" ;;
esac
note "PASS"
