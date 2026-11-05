#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
# Evidence must land in a path the caller can keep; the default is the
# gitignored build tree so a bare run works on any host.
root=${EVENT_STATE_EVIDENCE_ROOT:-"$repo/.kernel-build/host-evidence/event-state"}
run="$root/$(date -u +%Y%m%dT%H%M%SZ)-$$"
BUILD_DIR="$run/build"
USER_BUILD_DIR="$run/user"
evidence="$run/evidence"
rustc_bin=${RUSTC:-rustc}
mkdir -p "$BUILD_DIR" "$USER_BUILD_DIR" "$evidence"

common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir)
exec 9>"$common/parallel-rewrite/activity.lock"
flock -s 9
exec 8>"$common/parallel-rewrite/build-1.lock"
if ! flock -n 8; then
    exec 8>"$common/parallel-rewrite/build-2.lock"
    flock -n 8 || exit 75
fi

df -Pk / "$root" >"$evidence/disk.txt"
sha256sum "$repo/kernel/fs/eventpoll_state.rs" \
    "$repo/kernel/ipc/event_descriptor_state.rs" \
    "$repo/kernel/abi/linux/sys_pidfd.c" \
    "$repo/kernel/include/abi/linux/pidfd.h" \
    "$repo/kernel/ipc/signalfd.c" \
    "$repo/kernel/fs/eventpoll.c" "$repo/kernel/include/fs/eventpoll.h" \
    "$repo/kernel/fs/readiness.c" "$repo/kernel/include/fs/readiness.h" \
    "$repo/tools/host-tests/test-eventpoll-filter-host.c" \
    "$repo/user/cmds/stress/poll_edge.c" \
    "$repo/user/cmds/stress/syscall_ext.c" >"$evidence/source.sha256"
sha256sum "$0" >"$evidence/harness.sha256"
"$rustc_bin" --version --verbose >"$evidence/rustc-version.txt" 2>&1
printf '%s\n' \
    "$rustc_bin --edition=2021 --test $repo/kernel/fs/eventpoll_state.rs -o $BUILD_DIR/eventpoll-state-tests" \
    "$BUILD_DIR/eventpoll-state-tests" \
    "$rustc_bin --edition=2021 --test $repo/kernel/ipc/event_descriptor_state.rs -o $BUILD_DIR/event-descriptor-state-tests" \
    "$BUILD_DIR/event-descriptor-state-tests" >"$evidence/argv.txt"

status=0
for suite in eventpoll-state event-descriptor-state; do
    case "$suite" in
        eventpoll-state) source="$repo/kernel/fs/eventpoll_state.rs" ;;
        event-descriptor-state) source="$repo/kernel/ipc/event_descriptor_state.rs" ;;
    esac
    binary="$BUILD_DIR/$suite-tests"
    set +e
    "$rustc_bin" --edition=2021 --test "$source" -o "$binary" \
        >"$evidence/$suite.compile.log" 2>&1
    compile_status=$?
    set -e
    printf '%s\n' "$compile_status" >"$evidence/$suite.compile.exit"
    if [ "$compile_status" -ne 0 ]; then
        status=$compile_status
        break
    fi
    sha256sum "$binary" >"$evidence/$suite.binary.sha256"
    set +e
    "$binary" >"$evidence/$suite.run.log" 2>&1
    run_status=$?
    set -e
    printf '%s\n' "$run_status" >"$evidence/$suite.run.exit"
    if [ "$run_status" -ne 0 ]; then
        status=$run_status
        break
    fi
done

if [ "$status" -eq 0 ]; then
    rust_object="$BUILD_DIR/eventpoll-state.o"
    rust_argv="$rustc_bin --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu -C panic=abort -C opt-level=2 $repo/kernel/fs/eventpoll_state.rs -o $rust_object"
    printf '%s\n' "$rust_argv" >"$evidence/eventpoll-rust-object.argv"
    set +e
    "$rustc_bin" --edition=2021 --crate-type lib --emit=obj \
        --target x86_64-unknown-linux-gnu -C panic=abort -C opt-level=2 \
        "$repo/kernel/fs/eventpoll_state.rs" -o "$rust_object" \
        >"$evidence/eventpoll-rust-object.compile.log" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" >"$evidence/eventpoll-rust-object.compile.exit"
    if [ "$status" -eq 0 ]; then
        sha256sum "$rust_object" >"$evidence/eventpoll-rust-object.sha256"
        gcc --version >"$evidence/gcc-version.txt" 2>&1
        gcc_argv="gcc -std=gnu11 -fno-builtin -ffunction-sections -fdata-sections -D__KERNEL__ -Ikernel/include -Ikernel/arch/riscv64/include tools/host-tests/test-eventpoll-filter-host.c $rust_object -Wl,--gc-sections -o $BUILD_DIR/eventpoll-filter-host"
        printf '%s\n' "$gcc_argv" >"$evidence/eventpoll-filter-host.argv"
        set +e
        gcc -std=gnu11 -fno-builtin -ffunction-sections -fdata-sections \
            -D__KERNEL__ -I"$repo/kernel/include" \
            -I"$repo/kernel/arch/riscv64/include" \
            "$repo/tools/host-tests/test-eventpoll-filter-host.c" "$rust_object" \
            -Wl,--gc-sections -o "$BUILD_DIR/eventpoll-filter-host" \
            >"$evidence/eventpoll-filter-host.compile.log" 2>&1
        status=$?
        set -e
        printf '%s\n' "$status" >"$evidence/eventpoll-filter-host.compile.exit"
    fi
    if [ "$status" -eq 0 ]; then
        sha256sum "$BUILD_DIR/eventpoll-filter-host" \
            >"$evidence/eventpoll-filter-host.binary.sha256"
        set +e
        "$BUILD_DIR/eventpoll-filter-host" \
            >"$evidence/eventpoll-filter-host.run.log" 2>&1
        status=$?
        set -e
        printf '%s\n' "$status" >"$evidence/eventpoll-filter-host.run.exit"
    fi
fi

printf '%s\n' "$status" >"$evidence/actual.exit"
printf 'event-state evidence: %s\n' "$evidence"
cat "$evidence/source.sha256" "$evidence/harness.sha256" \
    "$evidence/rustc-version.txt" "$evidence/argv.txt"
for suite in eventpoll-state event-descriptor-state; do
    for suffix in compile.exit binary.sha256 run.exit run.log; do
        file="$evidence/$suite.$suffix"
        if [ -f "$file" ]; then
            printf '%s\n' "--- $file"
            cat "$file"
        fi
    done
done
for file in "$evidence/eventpoll-rust-object.compile.exit" \
    "$evidence/eventpoll-rust-object.sha256" \
    "$evidence/eventpoll-filter-host.compile.exit" \
    "$evidence/eventpoll-filter-host.binary.sha256" \
    "$evidence/eventpoll-filter-host.run.exit" \
    "$evidence/eventpoll-filter-host.run.log"; do
    if [ -f "$file" ]; then
        printf '%s\n' "--- $file"
        cat "$file"
    fi
done
printf 'actual_exit_status=%s\n' "$status"
exit "$status"
