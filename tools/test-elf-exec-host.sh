#!/bin/sh
# Host gate for the Rust ELF/exec decision core (kernel/mm/elf_core.rs).
#
# Run through the build-2 slot:
#   <repo>/.git/parallel-rewrite/run-build.sh build-2 \
#       sh tools/test-elf-exec-host.sh
#
# Covers, without any kernel build:
#   1. the in-crate unit tests (rustc --test, both errno conventions and the
#      new a20/kexec/commit FFI surface);
#   2. the malformed/truncated/overflowing ELF fixture suite
#      (tools/tests/elf-fixtures) replayed against the live
#      crate — the fixtures are regenerated from generate_fixtures.py, so no
#      binary blobs live in the tree;
#   3. the C-side ABI mirror (kernel/include/mm/elf_rust.h) compiling with
#      all _Static_assert size pins satisfied.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/a20-elf-exec.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

echo "[1/4] elf_core.rs unit tests"
rustc --edition=2021 --test "$repo/kernel/mm/elf_core.rs" -o "$tmp/elf_core_test"
"$tmp/elf_core_test"

echo "[2/4] malformed-ELF fixture replay"
python3 "$repo/tools/tests/elf-fixtures/generate_fixtures.py" \
    "$tmp/fixtures" > /dev/null
rustc --edition=2021 --crate-type lib --crate-name elf_core_snapshot \
    "$repo/kernel/mm/elf_core.rs" -o "$tmp/libelf_core_snapshot.rlib"
rustc --edition=2021 \
    --extern elf_core_snapshot="$tmp/libelf_core_snapshot.rlib" \
    "$repo/tools/tests/elf-fixtures/oracle-driver.rs" \
    -o "$tmp/oracle"
"$tmp/oracle" "$tmp/fixtures/fixtures" \
    "$tmp/fixtures/rust-expectations.tsv"

echo "[3/4] NOMMU allocation ownership helper"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$repo/kernel/include" \
    "$repo/tools/tests/elf_nommu_allocs_host.c" -o "$tmp/nommu_allocs_test"
"$tmp/nommu_allocs_test"

echo "[4/4] C ABI mirror (elf_rust.h static asserts)"
cat > "$tmp/hdrcheck.c" <<'EOF'
#include "mm/elf_rust.h"
int main(void) { return 0; }
EOF
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$repo/kernel/include" \
    "$tmp/hdrcheck.c" -o "$tmp/hdrcheck"

echo "ELF_EXEC host gate: PASS"
