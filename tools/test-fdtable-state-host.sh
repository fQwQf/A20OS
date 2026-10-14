#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=<build-dir>/secondary-linux-abi-host
run_dir="$build_root/fdtable-state/$(date -u +%Y%m%dT%H%M%SZ)-$$"
BUILD_DIR="$run_dir/build"
USER_BUILD_DIR="$run_dir/user"
mkdir -p "$BUILD_DIR" "$USER_BUILD_DIR"
common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir)
exec 9>"$common/parallel-rewrite/activity.lock"
flock -s 9
exec 8>"$common/parallel-rewrite/build-1.lock"
if ! flock -n 8; then
    exec 8>"$common/parallel-rewrite/build-2.lock"
    flock -n 8 || exit 75
fi
df -Pk / "$build_root" >"$run_dir/disk.txt"
sha256sum "$repo/kernel/fs/fdtable_state.rs" >"$run_dir/source.sha256"
sha256sum "$repo/tools/test-fdtable-state-host.sh" >"$run_dir/harness.sha256"
rustc --version --verbose >"$run_dir/rustc-version.txt" 2>&1
printf '%s\n' \
    "rustc --edition=2021 --test $repo/kernel/fs/fdtable_state.rs -o $BUILD_DIR/fdtable-state-tests" \
    >"$run_dir/argv.txt"
set +e
rustc --edition=2021 --test "$repo/kernel/fs/fdtable_state.rs" \
    -o "$BUILD_DIR/fdtable-state-tests" >"$run_dir/compile.log" 2>&1
compile_status=$?
set -e
printf '%s\n' "$compile_status" >"$run_dir/compile.exit"
cat "$run_dir/source.sha256" "$run_dir/harness.sha256"
sed -n '1,4p' "$run_dir/rustc-version.txt"
cat "$run_dir/argv.txt" "$run_dir/compile.log"
printf 'compile_exit_status=%s\n' "$compile_status"
if [ "$compile_status" -ne 0 ]; then
    exit "$compile_status"
fi
sha256sum "$BUILD_DIR/fdtable-state-tests" >"$run_dir/test-binary.sha256"
cat "$run_dir/test-binary.sha256"
set +e
"$BUILD_DIR/fdtable-state-tests" >"$run_dir/run.log" 2>&1
status=$?
set -e
printf '%s\n' "$status" >"$run_dir/run.exit"
cat "$run_dir/run.log"
printf 'actual_exit_status=%s\n' "$status"
exit "$status"
