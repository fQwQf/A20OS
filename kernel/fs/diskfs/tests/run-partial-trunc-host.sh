#!/bin/sh
# P2 host evidence runner: compare the checked C reference and Rust-owned
# partial-truncate paths on identical pristine e2fsprogs images. The checked
# C path already reclaims the spanning tail; the old unchecked helper's leak
# remains documented as the reason for the Rust production transaction.
#
# Evidence model:
#   base.img   fresh mke2fs image + one debugfs-written 16 KiB /single file
#   C run      harness built WITHOUT CONFIG_EXT4_RUST_EXTENT_TXN (checked C
#              reference path)
#   Rust run   harness built WITH CONFIG_EXT4_RUST_EXTENT_TXN + the Rust
#              transaction static library (production selector default)
# Both runs execute identical scenarios on fresh copies of the same seeded
# image; each asserts an exact +2 free-block delta for 16384 -> 4097, verifies
# every truncate result/size, requires C/R final block-count parity, and runs
# independent e2fsck checks. A separate Rust image injects one EIO and retries.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
work=${P2_HOST_WORK:-/tmp/opencode/p2-host}
rm -rf "$work"
mkdir -p "$work"

cd "$repo_root"

echo "== toolchain =="
cc --version | head -1
rustc --version
mke2fs -V 2>&1 | head -1
e2fsck -V 2>&1 | head -1

DISKFS_SRCS="kernel/fs/diskfs/ext4.c kernel/fs/diskfs/ext4_namei.c \
kernel/fs/diskfs/ext4_file.c kernel/fs/diskfs/ext4_journal.c \
kernel/fs/diskfs/ext4_csum.c kernel/fs/diskfs/ext4_sync.c \
kernel/fs/diskfs/ext4_xattr.c"
FSCOMPAT_SRCS="user/svc/fscompat/compat.c user/svc/fscompat/bcache_user.c"
HOST_INC="-Iuser/svc/fscompat -Ikernel/include -Ikernel/arch/riscv64/include"

echo "== pristine fixture =="
truncate -s 32M "$work/base.img"
# The hosted fscompat adapter does not replay this e2fsprogs journal format;
# use an explicitly clean no-journal ext4 image for the independent allocator
# and e2fsck check. Journaled rollback/poison behavior is covered by Rust tests.
mke2fs -q -F -t ext4 -b 4096 -O ^has_journal "$work/base.img"
# 16 KiB contiguous pattern file -> /single is one 4-block extent.
awk 'BEGIN { for (i = 0; i < 16384; i++) printf "%c", (i % 251) + 1 }' \
    > "$work/pattern.bin"
debugfs -w -R "write $work/pattern.bin single" "$work/base.img" > /dev/null 2>&1
sha256sum "$work/base.img" | tee "$work/base.sha256"
e2fsck -fn "$work/base.img" > "$work/base.e2fsck" 2>&1 || {
    echo "seed image fails e2fsck:"; cat "$work/base.e2fsck"; exit 1; }
dumpe2fs -h "$work/base.img" 2>/dev/null | grep -E "^Free blocks" \
    | tee "$work/base.free"

echo "== build harness (C reference selector) =="
cc -std=gnu11 -O1 -ffunction-sections -fdata-sections -Wno-builtin-declaration-mismatch \
    $HOST_INC $DISKFS_SRCS $FSCOMPAT_SRCS kernel/fs/diskfs/tests/ext4_partial_trunc_host.c \
    -Wl,--gc-sections -o "$work/harness-c"

echo "== build Rust transaction static library =="
rustc --edition 2021 --crate-type staticlib --cfg host_std \
    -C opt-level=1 kernel/fs/diskfs/ext4_extent_txn.rs \
    -o "$work/libext4_extent_txn.a"

echo "== build harness (Rust selector) =="
cc -std=gnu11 -O1 -DCONFIG_EXT4_RUST_EXTENT_TXN \
    -ffunction-sections -fdata-sections -Wno-builtin-declaration-mismatch \
    $HOST_INC $DISKFS_SRCS $FSCOMPAT_SRCS kernel/fs/diskfs/tests/ext4_partial_trunc_host.c \
    "$work/libext4_extent_txn.a" \
    -Wl,--gc-sections -o "$work/harness-rust"

echo "== build isolated Rust failure-injection harness =="
cc -std=gnu11 -O1 -DCONFIG_EXT4_RUST_EXTENT_TXN \
    -DCONFIG_EXT4_TEST_TRUNC_FREE_FAIL_ONCE \
    -ffunction-sections -fdata-sections -Wno-builtin-declaration-mismatch \
    $HOST_INC $DISKFS_SRCS $FSCOMPAT_SRCS kernel/fs/diskfs/tests/ext4_partial_trunc_host.c \
    "$work/libext4_extent_txn.a" \
    -Wl,--gc-sections -o "$work/harness-rust-fault"

echo "== Rust unit tests (transaction, incl. failure paths) =="
rustc --test --edition=2021 kernel/fs/diskfs/ext4_extent_txn.rs \
    -o "$work/ext4-extent-txn-tests"
"$work/ext4-extent-txn-tests" > "$work/rust-unit-tests.log" 2>&1
cat "$work/rust-unit-tests.log"

run_scenarios() {
    # $1 = harness binary, $2 = image
    "$1" "$2" \
        statfs \
        trunc:single:4097 \
        statfs \
        trunc:single:4096 \
        trunc:single:0 \
        mkfile:sparse:0,16 \
        trunc:sparse:4096 \
        mkfile:deep:0,4,8,12 \
        trunc:deep:4096 \
        statfs
}

for sel in c rust; do
    echo "== run selector=$sel =="
    cp "$work/base.img" "$work/work-$sel.img"
    sha256sum "$work/work-$sel.img" > "$work/work-$sel.pre.sha256"
    run_scenarios "$work/harness-$sel" "$work/work-$sel.img" \
        > "$work/run-$sel.log" 2>&1
    cat "$work/run-$sel.log"
    grep -q '^HOST_EXPECTED_RECLAIM_PASS delta=2$' "$work/run-$sel.log"
    grep -q '^HOST_SCENARIOS_PASS$' "$work/run-$sel.log"
    grep -q '^HOST_CONTENT_PASS$' "$work/run-$sel.log"
    sha256sum "$work/work-$sel.img" > "$work/work-$sel.post.sha256"
    if ! e2fsck -fn "$work/work-$sel.img" > "$work/e2fsck-$sel.log" 2>&1; then
        echo "e2fsck-$sel: DIRTY (see $work/e2fsck-$sel.log)"
        cat "$work/e2fsck-$sel.log"
        exit 1
    fi
    echo "e2fsck-$sel: clean"
    dumpe2fs -h "$work/work-$sel.img" 2>/dev/null | grep -E "^Free blocks" \
        | tee "$work/free-$sel.txt"
done

echo "== desktop sparse writeback and fast-symlink unlink =="
for sel in c rust; do
    cp "$work/base.img" "$work/desktop-$sel.img"
    "$work/harness-$sel" "$work/desktop-$sel.img" desktop writeback-eof \
        > "$work/desktop-$sel.log" 2>&1
    cat "$work/desktop-$sel.log"
    grep -q '^HOST_DESKTOP_BATCH_AND_SYMLINK_PASS$' "$work/desktop-$sel.log"
    grep -q '^HOST_WRITEBACK_EOF_RACE_PASS$' "$work/desktop-$sel.log"
    e2fsck -fn "$work/desktop-$sel.img" > "$work/desktop-$sel.e2fsck" 2>&1
    cp "$work/base.img" "$work/shutdown-$sel.img"
    "$work/harness-$sel" "$work/shutdown-$sel.img" desktop-shutdown \
        > "$work/shutdown-$sel.log" 2>&1
    grep -q '^HOST_DESKTOP_SHUTDOWN_PASS$' "$work/shutdown-$sel.log"
    e2fsck -fn "$work/shutdown-$sel.img" > "$work/shutdown-$sel.e2fsck" 2>&1
done

# Simulate a crash after unlink published an orphaned Firefox-style lock.
# Mount recovery must drain its inline target without treating it as extents.
cp "$work/base.img" "$work/orphan-symlink.img"
debugfs -w -R 'symlink lock 127.0.1.1:+189' "$work/orphan-symlink.img" >/dev/null 2>&1
ino=$(debugfs -R 'stat lock' "$work/orphan-symlink.img" 2>/dev/null | \
    sed -n 's/^Inode: \([0-9]*\).*/\1/p')
test -n "$ino"
debugfs -w -R "set_inode_field lock links_count 0" "$work/orphan-symlink.img" >/dev/null 2>&1
debugfs -w -R 'unlink lock' "$work/orphan-symlink.img" >/dev/null 2>&1
debugfs -w -R "set_super_value last_orphan $ino" "$work/orphan-symlink.img" >/dev/null 2>&1
debugfs -w -R 'set_super_value state 0' "$work/orphan-symlink.img" >/dev/null 2>&1
for sel in c rust; do
    cp "$work/orphan-symlink.img" "$work/orphan-symlink-$sel.img"
    "$work/harness-$sel" "$work/orphan-symlink-$sel.img" statfs \
        > "$work/orphan-symlink-$sel.log" 2>&1
    e2fsck -fn "$work/orphan-symlink-$sel.img" > "$work/orphan-symlink-$sel.e2fsck" 2>&1
    echo "orphan-symlink-$sel: recovery and e2fsck PASS"
done

echo "== Rust depth-1 input and reclaim validation =="
cp "$work/base.img" "$work/work-rust-depth1.img"
"$work/harness-rust" "$work/work-rust-depth1.img" \
    mkfile:depth1:0,4,8,12,16 statfs expectdelta:5 \
    trunc:depth1:4096 statfs > "$work/run-rust-depth1.log" 2>&1
cat "$work/run-rust-depth1.log"
grep -q '^HOST_DEPTH1_PRECONDITION_PASS$' "$work/run-rust-depth1.log"
grep -q '^HOST_EXPECTED_RECLAIM_PASS delta=5$' "$work/run-rust-depth1.log"
grep -q '^HOST_SCENARIOS_PASS$' "$work/run-rust-depth1.log"
if ! e2fsck -fn "$work/work-rust-depth1.img" > "$work/e2fsck-rust-depth1.log" 2>&1; then
    cat "$work/e2fsck-rust-depth1.log"
    exit 1
fi
echo "e2fsck-rust-depth1: clean"
dumpe2fs -h "$work/work-rust-depth1.img" 2>/dev/null | grep -E "^Free blocks" \
    | tee "$work/free-rust-depth1.txt"
depth1_free=$(awk '{print $3}' "$work/free-rust-depth1.txt")
depth1_before=$(sed -n 's/^STATFS free=\([0-9][0-9]*\).*/\1/p' \
    "$work/run-rust-depth1.log" | head -1)
if [ "$depth1_free" -ne "$((depth1_before + 5))" ]; then
    echo "Rust depth-1 free blocks mismatch: before=$depth1_before actual=$depth1_free expected=$((depth1_before + 5))"
    exit 1
fi

echo "== isolated Rust EIO / retry validation =="
cp "$work/base.img" "$work/work-rust-fault.img"
"$work/harness-rust-fault" "$work/work-rust-fault.img" \
    trunc-retry:single:4097:16384 > "$work/run-rust-fault.log" 2>&1
cat "$work/run-rust-fault.log"
grep -q '^HOST_RETRY_PASS$' "$work/run-rust-fault.log"
grep -q '^HOST_RETRY_OLD_CONTENT_PASS$' "$work/run-rust-fault.log"
grep -q '^HOST_SCENARIOS_PASS$' "$work/run-rust-fault.log"
e2fsck -fn "$work/work-rust-fault.img" > "$work/e2fsck-rust-fault.log" 2>&1 || {
    cat "$work/e2fsck-rust-fault.log"
    exit 1
}
echo "e2fsck-rust-fault: clean"
sha256sum "$work/work-rust-fault.img" | tee "$work/work-rust-fault.sha256"

echo "== comparison =="
free_c=$(awk '{print $3}' "$work/free-c.txt")
free_rust=$(awk '{print $3}' "$work/free-rust.txt")
echo "free blocks  C selector: $free_c"
echo "free blocks  Rust selector: $free_rust"
if [ "$free_rust" -eq "$free_c" ]; then
    echo "RESULT: checked C and Rust final free-block counts match ($free_c);" \
         "both passed the independent +2 partial-reclaim assertion and e2fsck"
else
    echo "RESULT: C/R final free-block counts differ (unexpected)"
    exit 1
fi
echo "evidence dir: $work"

# APK mutates imported indexed /usr/bin-like directories and can allocate
# into a previously untouched lazy block/inode group. Check both selectors
# against independently rebuilt e2fsprogs indexes and final fsck.
mkdir -p "$work/index-seed/indexed"
i=1
while [ "$i" -le 512 ]; do
    name=$(printf 'old-%06d' "$i")
    : > "$work/index-seed/indexed/$name"
    i=$((i + 1))
done
truncate -s 512M "$work/index-base.img"
mke2fs -q -F -t ext4 -b 4096 -O ^has_journal -d "$work/index-seed" "$work/index-base.img"
e2fsck -fyD "$work/index-base.img" > "$work/index-optimize.log" 2>&1 || [ "$?" -eq 1 ]
for selector in c rust; do
    cp --sparse=always "$work/index-base.img" "$work/index-$selector.img"
    "$work/harness-$selector" "$work/index-$selector.img" indexed-lazy \
        > "$work/index-$selector.log" 2>&1
    e2fsck -fn "$work/index-$selector.img" > "$work/index-$selector.e2fsck" 2>&1
    echo "indexed directory + lazy allocation ($selector): fsck clean"
done
