#!/usr/bin/env bash
#
# Build the architecture-specific stage-1 probes against the glibc shipped in
# the official read-only image.  Only libc.so.6 is extracted, read-only, into
# the per-run directory; the published image itself is never modified.

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 riscv64|loongarch64 OFFICIAL_BASE.img OUTPUT_DIR" >&2
    exit 2
fi

arch=$1
base_image=$2
output_dir=$3

case "$arch" in
riscv64)
    cc=riscv64-linux-gnu-gcc
    libc_path=/lib/riscv64-linux-gnu/libc.so.6
    dynamic_loader=/lib/ld-linux-riscv64-lp64d.so.1
    ;;
loongarch64)
    cc=loongarch64-linux-gnu-gcc
    libc_path=/usr/lib/loongarch64-linux-gnu/libc.so.6
    dynamic_loader=/lib64/ld-linux-loongarch-lp64d.so.1
    ;;
*)
    echo "[buildstorm-probe] unsupported architecture: $arch" >&2
    exit 2
    ;;
esac

for command_name in debugfs "$cc"; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "[buildstorm-probe] missing required command: $command_name" >&2
        exit 127
    fi
done
if [[ ! -r "$base_image" ]]; then
    echo "[buildstorm-probe] unreadable official base: $base_image" >&2
    exit 1
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(cd "$script_dir/.." && pwd -P)
source_dir="$repo_root/user/probes/buildstorm"

mkdir -p "$output_dir/objects"
libc_copy="$output_dir/objects/libc.so.6"
debugfs -R "dump -p $libc_path $libc_copy" "$base_image" >/dev/null 2>&1
if [[ ! -s "$libc_copy" ]]; then
    echo "[buildstorm-probe] failed to extract $libc_path" >&2
    exit 1
fi

common_cflags=(
    -O2 -fno-stack-protector -fno-builtin -ffreestanding
    -fno-asynchronous-unwind-tables -fno-unwind-tables
)
"$cc" "${common_cflags[@]}" -fPIE -c \
    "$source_dir/cwd_probe.c" -o "$output_dir/objects/cwd_probe.o"
"$cc" -fPIE -c "$source_dir/probe_start.S" \
    -o "$output_dir/objects/probe_start.o"
"$cc" -nostdlib -pie -Wl,-z,now -Wl,--allow-shlib-undefined \
    -Wl,--dynamic-linker="$dynamic_loader" \
    -o "$output_dir/cwd-probe" \
    "$output_dir/objects/probe_start.o" \
    "$output_dir/objects/cwd_probe.o" \
    "$libc_copy"

"$cc" "${common_cflags[@]}" -fPIC -c \
    "$source_dir/exec_pages_dso.c" \
    -o "$output_dir/objects/exec_pages_dso.o"
"$cc" -nostdlib -shared -Wl,-soname,liba20probe.so \
    -o "$output_dir/liba20probe.so" \
    "$output_dir/objects/exec_pages_dso.o"

"$cc" "${common_cflags[@]}" -fPIE -c \
    "$source_dir/exec_pages_main.c" \
    -o "$output_dir/objects/exec_pages_main.o"
"$cc" -nostdlib -pie -Wl,-z,now \
    -Wl,--dynamic-linker="$dynamic_loader" \
    -Wl,-rpath,/a20-probe \
    -o "$output_dir/exec-pages-probe" \
    "$output_dir/objects/probe_start.o" \
    "$output_dir/objects/exec_pages_main.o" \
    -L"$output_dir" -la20probe

"$cc" "${common_cflags[@]}" -fPIE -c \
    "$source_dir/shebang_exec_probe.c" \
    -o "$output_dir/objects/shebang_exec_probe.o"
"$cc" -nostdlib -pie -Wl,-z,now -Wl,--allow-shlib-undefined \
    -Wl,--dynamic-linker="$dynamic_loader" \
    -o "$output_dir/shebang-probe" \
    "$output_dir/objects/probe_start.o" \
    "$output_dir/objects/shebang_exec_probe.o" \
    "$libc_copy"

"$cc" "${common_cflags[@]}" -fPIE -c \
    "$source_dir/stage9_perf_probe.c" \
    -o "$output_dir/objects/stage9_perf_probe.o"
"$cc" -nostdlib -pie -Wl,-z,now -Wl,--allow-shlib-undefined \
    -Wl,--dynamic-linker="$dynamic_loader" \
    -o "$output_dir/stage9-perf-probe" \
    "$output_dir/objects/probe_start.o" \
    "$output_dir/objects/stage9_perf_probe.o" \
    "$libc_copy"

chmod 0755 "$output_dir/cwd-probe" "$output_dir/exec-pages-probe" \
    "$output_dir/shebang-probe" "$output_dir/stage9-perf-probe"
chmod 0644 "$output_dir/liba20probe.so"

echo "[buildstorm-probe] built $arch probes in $output_dir"
