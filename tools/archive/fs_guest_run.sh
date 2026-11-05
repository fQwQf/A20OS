#!/bin/sh
# Run one bound FS-frontend guest workload.
#
# usage: tools/fs_guest_run.sh NAME KERNEL.ELF LOGFILE CMDFILE
#   NAME      variant label used for the evidence header
#   KERNEL.ELF the kernel under test (the only thing that differs between
#              variants -- userland and disk inputs are copied byte-for-byte)
#   LOGFILE   where the guest console goes
#   CMDFILE   one guest command per line; each is sent after the shell prompt
#              that follows the previous command
#
# Environment:
#   FS_GUEST_DIR  root holding kernel/{fat32,isofs}.img and
#                 bisect/ext4-pristine.img  (default: ../fs-frontend-run)
#   GUEST_TIMEOUT qemu duration (default 180s)
#   GUEST_QEMU    qemu binary (default qemu-system-riscv64)
#
# Before the run the FAT32 and ext4 inputs are copied into a private scratch
# directory, so two variants see byte-identical images even when the guest
# writes to them.  The header records the source commit, the dirty-diff hash,
# every input hash, the QEMU argv and the natural exit status; the summary
# line reports the ext4 unlink-orphan deferral count and any FAIL marker.
set -eu

repo=$(git rev-parse --show-toplevel)
LOCKDIR=$repo/.git/parallel-rewrite
name=${1:?usage: fs_guest_run.sh NAME KERNEL LOGFILE CMDFILE}
kernel=${2:?kernel elf}
log=${3:?log file}
cmds=${4:?command file}

fsdir=${FS_GUEST_DIR:-$repo/../fs-frontend-run}
fsdir=$(cd "$fsdir" && pwd)
timeout_s=${GUEST_TIMEOUT:-180}
qemu=${GUEST_QEMU:-qemu-system-riscv64}

scratch=$fsdir/scratch
rm -rf "$scratch"
mkdir -p "$scratch"
cp "$fsdir/kernel/fat32.img" "$scratch/fat32.img"
cp "$fsdir/bisect/ext4-pristine.img" "$scratch/ext4.img"
cp "$fsdir/kernel/isofs.img" "$scratch/isofs.img"

h() { sha256sum "$1" | cut -d' ' -f1; }
dirty=$(git -C "$repo" diff HEAD | sha256sum | cut -d' ' -f1)
argv="$qemu -machine virt -m 1G -nographic -smp 2 -bios default -global virtio-mmio.force-legacy=false -drive file=$scratch/fat32.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 -drive file=$scratch/ext4.img,if=none,format=raw,id=x1 -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 -drive file=$scratch/isofs.img,if=none,format=raw,id=x2 -device virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2 -netdev user,id=net -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 -kernel $kernel"

{
    echo "variant=$name"
    echo "source_commit=$(git -C "$repo" rev-parse HEAD)"
    echo "dirty_diff_sha256=$dirty"
    echo "kernel_sha256=$(h "$kernel")"
    echo "fat32_in_sha256=$(h "$scratch/fat32.img")"
    echo "ext4_in_sha256=$(h "$scratch/ext4.img")"
    echo "isofs_in_sha256=$(h "$scratch/isofs.img")"
    echo "ext4_s_state=$(od -An -tx1 -j 1082 -N 2 "$scratch/ext4.img" | tr -d ' ')"
    echo "argv=$argv"
    echo "commands=$(tr '\n' ';' < "$cmds")"
} > "$log.header"

# activity SH + qemu-1 EX, bounded backoff (exit 75 == resource busy).
tries=0
while :; do
    exec 9>"$LOCKDIR/activity.lock"
    flock -s 9
    if exec 8>"$LOCKDIR/qemu-1.lock" && flock -E 75 -n 8; then
        break
    fi
    exec 9>&- 8>&-
    tries=$((tries + 1))
    if [ "$tries" -ge "${FS_GUEST_TRIES:-40}" ]; then
        echo "fs_guest_run: qemu-1 busy after $tries tries" >&2
        exit 75
    fi
    sleep 15
done

rc=0
set -- --expect-line '# ' "$(sed -n '1p' "$cmds")"
n=$(wc -l < "$cmds")
i=1
while [ "$i" -lt "$n" ]; do
    i=$((i + 1))
    set -- "$@" --expect-line '# ' "$(sed -n "${i}p" "$cmds")"
done
python3 "$repo/tools/run_with_timeout.py" "$@" "$timeout_s" \
    $argv > "$log" 2>&1 || rc=$?

exec 8>&- 9>&-

echo "exit_status=$rc" >> "$log.header"
echo "ext4_out_sha256=$(h "$scratch/ext4.img")" >> "$log.header"
echo "ext4_out_s_state=$(od -An -tx1 -j 1082 -N 2 "$scratch/ext4.img" | tr -d ' ')" >> "$log.header"
echo "fat32_out_sha256=$(h "$scratch/fat32.img")" >> "$log.header"

orphans=$(grep -c 'unlink orphan ino=' "$log" || true)
fails=$(grep -cE '(^|[^A-Za-z_])FAIL[^A-Za-z_]' "$log" || true)
powered=$(grep -c 'POWER_OFF entry' "$log" || true)
echo "orphan_deferrals=$orphans"
echo "fail_markers=$fails"
echo "poweroff_markers=$powered"
grep -o 'VFS_BENCH: [^ ]*' "$log" | sort -u | sed 's/^/marker: /' || true
echo "log=$log"
# A guest that never reached its own power-off must not be read as success:
# poweroff must not mask a failed workload.
if [ "$rc" -eq 124 ]; then
    echo "fs_guest_run: timeout without a natural exit" >&2
    exit 124
fi
if [ "$powered" -eq 0 ]; then
    echo "fs_guest_run: no POWER_OFF entry -- the guest did not exit naturally" >&2
    exit 1
fi
if [ "$fails" -ne 0 ]; then
    echo "fs_guest_run: FAIL marker present in the guest log" >&2
    exit 1
fi
[ "$rc" -eq 0 ] || exit "$rc"
