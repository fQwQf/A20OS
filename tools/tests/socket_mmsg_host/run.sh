#!/usr/bin/env bash
set -u

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd -P)
common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir)
if [[ $# -ne 1 || $1 != /* ]]; then
    echo "usage: $0 ABSOLUTE_NEW_EVIDENCE_DIR" >&2
    exit 64
fi
out=$1
mkdir -p "$(dirname "$out")"
if ! mkdir "$out"; then
    echo "evidence directory already exists: $out" >&2
    exit 73
fi

exec 9>"$common/parallel-rewrite/activity.lock"
flock -s 9
exec 8>"$common/parallel-rewrite/build-1.lock"
if ! flock -n 8; then
    exec 8>"$common/parallel-rewrite/build-2.lock"
    if ! flock -n 8; then
        echo "no free build lease" >"$out/lease.error"
        exit 75
    fi
fi

cd "$repo"
printf '%q ' "$0" "$out" >"$out/runner.argv"
printf '\n' >>"$out/runner.argv"
date -u +'%Y-%m-%dT%H:%M:%SZ' >"$out/start.utc"
df -h "$repo" >"$out/df.before.txt"
rustc --version --verbose >"$out/rustc.txt" 2>&1
gcc --version >"$out/gcc.txt" 2>&1
sha256sum tools/tests/socket_mmsg_host/run.sh \
    tools/tests/socket_mmsg_host/linux_oracle.c \
    kernel/abi/linux/mmsg_transaction.rs \
    kernel/include/abi/linux/mmsg_transaction.h \
    kernel/abi/linux/sys_socket_msg.c kernel/net/socket/socket_control.c \
    user/cmds/stress/socket_stress.c >"$out/inputs.before.sha256"

set +e
rustc --edition=2021 --test kernel/abi/linux/mmsg_transaction.rs \
    -o "$out/mmsg_tests" >"$out/rustc.log" 2>&1
rc=$?
printf '%s\n' "$rc" >"$out/rustc.exit"
printf '%q ' rustc --edition=2021 --test kernel/abi/linux/mmsg_transaction.rs \
    -o "$out/mmsg_tests" >"$out/rustc.argv"
printf '\n' >>"$out/rustc.argv"
if [[ $rc -eq 0 ]]; then
    "$out/mmsg_tests" --nocapture >"$out/rust-test.log" 2>&1
    rc=$?
else
    rc=125
fi
printf '%s\n' "$rc" >"$out/rust-test.exit"

gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
    tools/tests/socket_mmsg_host/linux_oracle.c -o "$out/linux_oracle" \
    >"$out/oracle-compile.log" 2>&1
rc=$?
printf '%s\n' "$rc" >"$out/oracle-compile.exit"
printf '%q ' gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
    tools/tests/socket_mmsg_host/linux_oracle.c -o "$out/linux_oracle" \
    >"$out/oracle-compile.argv"
printf '\n' >>"$out/oracle-compile.argv"
if [[ $rc -eq 0 ]]; then
    "$out/linux_oracle" >"$out/oracle.log" 2>&1
    rc=$?
else
    rc=125
fi
printf '%s\n' "$rc" >"$out/oracle.exit"

gcc -std=gnu11 -Wall -Wextra -Werror -fsyntax-only \
    user/cmds/stress/socket_stress.c >"$out/user-syntax.log" 2>&1
rc=$?
printf '%s\n' "$rc" >"$out/user-syntax.exit"
printf '%q ' gcc -std=gnu11 -Wall -Wextra -Werror -fsyntax-only \
    user/cmds/stress/socket_stress.c >"$out/user-syntax.argv"
printf '\n' >>"$out/user-syntax.argv"

sha256sum tools/tests/socket_mmsg_host/run.sh \
    tools/tests/socket_mmsg_host/linux_oracle.c \
    kernel/abi/linux/mmsg_transaction.rs \
    kernel/include/abi/linux/mmsg_transaction.h \
    kernel/abi/linux/sys_socket_msg.c kernel/net/socket/socket_control.c \
    user/cmds/stress/socket_stress.c >"$out/inputs.after.sha256"
if [[ -e "$out/mmsg_tests" ]]; then sha256sum "$out/mmsg_tests"; fi >"$out/binaries.sha256"
if [[ -e "$out/linux_oracle" ]]; then sha256sum "$out/linux_oracle"; fi >>"$out/binaries.sha256"
df -h "$repo" >"$out/df.after.txt"
date -u +'%Y-%m-%dT%H:%M:%SZ' >"$out/end.utc"

status=0
for f in rustc.exit rust-test.exit oracle-compile.exit oracle.exit user-syntax.exit; do
    value=$(cat "$out/$f")
    [[ $value == 0 ]] || status=1
done
printf '%s\n' "$status" >"$out/overall.exit"
exit "$status"
