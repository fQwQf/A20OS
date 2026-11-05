#!/bin/sh
# Build identical static musl probes for Linux and A20 x86_64 guests.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_out=${1:-"$repo/build/fork-fast"}
probe_cc=${CC:-gcc}
probe_musl="$repo/user/external/musl"
probe_lib="$probe_musl/build-x86_64/lib"
probe_include=$("$probe_cc" -print-file-name=include)
mkdir -p "$probe_out"
for probe in fork_exit_guest fork_exit_bench; do
    case "$probe" in
        fork_exit_guest) probe_name=fork-exit-guest ;;
        fork_exit_bench) probe_name=fork-exit-bench ;;
    esac
    "$probe_cc" -O2 -static -nostdlib -nostdinc -fno-stack-protector -no-pie \
        -I "$probe_musl/build-x86_64/obj/include" -I "$probe_musl/arch/x86_64" \
        -I "$probe_musl/arch/generic" -I "$probe_musl/include" -isystem "$probe_include" \
        "$probe_lib/crt1.o" "$probe_lib/crti.o" "$repo/tools/tests/$probe.c" \
        "$probe_lib/libc.a" -lgcc "$probe_lib/crtn.o" -o "$probe_out/$probe_name"
done
