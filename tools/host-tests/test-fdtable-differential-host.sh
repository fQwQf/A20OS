#!/bin/sh
# Host harness for kernel/fs/fdtable.c against the Rust fdtable state object.
#
# This used to build each mode twice ("c" and "rust") and compare.  The C
# implementation was deleted when the state moved to fdtable_state.rs, so the
# "c" variant no longer links: kernel/fs/fdtable.c now references the
# a20_fdtable_*_rust entry points unconditionally.  Only the production
# configuration is built here.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
# Evidence must land in a path the caller can keep; the default is the
# gitignored build tree so a bare run works on any host.
root=${FDTABLE_HOST_EVIDENCE_ROOT:-"$repo/.kernel-build/host-evidence/fdtable-differential"}
run="$root/fdtable-differential/$(date -u +%Y%m%dT%H%M%SZ)-$$"
BUILD_DIR="$run/build"
USER_BUILD_DIR="$run/user"
shims="$repo/tools/tests/fdtable_host/include"
mkdir -p "$BUILD_DIR" "$USER_BUILD_DIR" "$run/evidence"
ev="$run/evidence"

common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir)
exec 9>"$common/parallel-rewrite/activity.lock"
flock -s 9
exec 8>"$common/parallel-rewrite/build-2.lock"
flock -n 8 || exit 75

mkdir -p "$run/tmp"
export TMPDIR="$run/tmp"

df -Pk / "$root" >"$ev/disk.txt"
write_inputs() {
    sha256sum "$repo/kernel/fs/fdtable.c" "$repo/kernel/include/fs/fdtable.h" \
        "$repo/kernel/fs/fdtable_state.rs" "$repo/tools/tests/fdtable_host/differential.c"
    find "$repo/tools/tests/fdtable_host/include" -type f -print0 |
        LC_ALL=C sort -z | xargs -0 sha256sum
    sha256sum "$0"
}
write_inputs >"$ev/inputs.before.sha256"
gcc --version >"$ev/gcc-version.txt" 2>&1
rustc --version --verbose >"$ev/rustc-version.txt" 2>&1

status=0
for mode in rust; do
    variant="$BUILD_DIR/$mode"
    mkdir -p "$variant"
    ev_mode="$ev/$mode"
    mkdir -p "$ev_mode"
    rust_obj=""
    define=""
    if [ "$mode" = rust ]; then
        rust_obj="$variant/fdtable_state.o"
        define="-DCONFIG_FDTABLE_STATE_RUST"
        rust_argv="rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu -C panic=abort -C opt-level=2 $repo/kernel/fs/fdtable_state.rs -o $rust_obj"
        printf '%s\n' "$rust_argv" >"$ev_mode/rust-argv.txt"
        set +e
        rustc --edition=2021 --crate-type lib --emit=obj \
            --target x86_64-unknown-linux-gnu -C panic=abort -C opt-level=2 \
            "$repo/kernel/fs/fdtable_state.rs" -o "$rust_obj" \
            >"$ev_mode/rust-compile.log" 2>&1
        rust_status=$?
        set -e
        printf '%s\n' "$rust_status" >"$ev_mode/rust-compile.exit"
        if [ "$rust_status" -ne 0 ]; then
            cat "$ev_mode/rust-compile.log"
            status=$rust_status
            break
        fi
        sha256sum "$rust_obj" >"$ev_mode/rust-object.sha256"
    fi

    c_argv="gcc -std=gnu99 -O2 -pthread -ffunction-sections -fdata-sections -ffreestanding -fno-builtin -DFDTABLE_HOST_TEST $define -I$shims -I$repo/kernel/include -I$repo/kernel $repo/tools/tests/fdtable_host/differential.c $repo/kernel/fs/fdtable.c $rust_obj -Wl,--gc-sections -o $variant/fdtable-differential"
    printf '%s\n' "$c_argv" >"$ev_mode/argv.txt"
    set +e
    gcc -std=gnu99 -O2 -pthread -ffunction-sections -fdata-sections \
        -ffreestanding -fno-builtin -DFDTABLE_HOST_TEST $define -I"$shims" \
        -I"$repo/kernel/include" -I"$repo/kernel" \
        "$repo/tools/tests/fdtable_host/differential.c" \
        "$repo/kernel/fs/fdtable.c" $rust_obj -Wl,--gc-sections \
        -o "$variant/fdtable-differential" >"$ev_mode/compile.log" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" >"$ev_mode/compile.exit"
    if [ "$status" -ne 0 ]; then
        cat "$ev_mode/compile.log"
        break
    fi
    sha256sum "$variant/fdtable-differential" >"$ev_mode/binary.sha256"
    set +e
    "$variant/fdtable-differential" >"$ev_mode/run.log" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" >"$ev_mode/run.exit"
    cat "$ev_mode/run.log"
    if [ "$status" -ne 0 ]; then
        break
    fi
done

write_inputs >"$ev/inputs.after.sha256"
if ! cmp -s "$ev/inputs.before.sha256" "$ev/inputs.after.sha256"; then
    printf '%s\n' 'source_inputs_changed_during_run=1' >"$ev/source-drift.exit"
    status=80
else
    printf '%s\n' 'source_inputs_changed_during_run=0' >"$ev/source-drift.exit"
fi
cat "$ev/inputs.before.sha256" "$ev/inputs.after.sha256" \
    "$ev/source-drift.exit" "$ev/gcc-version.txt" "$ev/rustc-version.txt"
for mode in rust; do
    test ! -f "$ev/$mode/argv.txt" || cat "$ev/$mode/argv.txt"
    test ! -f "$ev/$mode/compile.log" || cat "$ev/$mode/compile.log"
    test ! -f "$ev/$mode/compile.exit" || cat "$ev/$mode/compile.exit"
    test ! -f "$ev/$mode/run.exit" || cat "$ev/$mode/run.exit"
    test ! -f "$ev/$mode/binary.sha256" || cat "$ev/$mode/binary.sha256"
done
printf 'actual_exit_status=%s\n' "$status"
exit "$status"
