#!/bin/bash
# RV64 guest evidence run for the conntrack/NAT migration.
#
# Boots the production kernel, lets the guest install a DNAT rule and listen on
# the translated port, and drives real host->guest traffic through QEMU's hostfwd
# so the translation is exercised end to end: the reply the host receives can
# only carry the forwarded source port if the reverse direction was rewritten
# from the conntrack entry.  Then the filter/conntrack path runs and the guest is
# asked for its status before a clean poweroff.
#
# Usage: run.sh EVIDENCE_DIR KERNEL_ELF FAT32_IMG [NR_CPUS]
# Locks: activity SH + qemu-1 EX (one VM globally, <=1 GiB).  Exit 75 = busy.
set -uo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo "usage: $0 EVIDENCE_DIR KERNEL_ELF FAT32_IMG [NR_CPUS]" >&2
  exit 64
fi
out=$1
kernel=$2
image=$3
smp=${4:-2}
if [[ "$out" != /* || "$kernel" != /* || "$image" != /* ]]; then
  echo "usage: $0 ABSOLUTE paths" >&2
  exit 64
fi
mkdir -p "$(dirname -- "$out")"
if ! mkdir "$out"; then
  echo "evidence directory already exists or cannot be created: $out" >&2
  exit 73
fi

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir) || exit 74
lockdir="$common/parallel-rewrite"

record_exit() { printf '%s %s\n' "$1" "$2" >> "$out/exits.txt"; }

printf '%q ' "$0" "$@" > "$out/run.argv"
printf '\n' >> "$out/run.argv"

exec 9>"$lockdir/activity.lock" || exit 1
flock -s 9
record_exit activity-shared 0
exec 8>"$lockdir/qemu-1.lock" || exit 1
if ! flock -w 2 -x 8; then
  record_exit qemu-slot 75
  exit 75
fi
record_exit qemu-slot 0

df -h / > "$out/disk.before.txt" 2>&1
{
  echo "source_commit=$(git -C "$repo" rev-parse HEAD)"
  echo "source_dirty_diff_sha256=$(git -C "$repo" diff | sha256sum | cut -d' ' -f1)"
  echo "source_status_sha256=$(git -C "$repo" status --porcelain | sha256sum | cut -d' ' -f1)"
} > "$out/source.txt" 2>&1
sha256sum "$kernel" "$image" > "$out/images.sha256" 2>&1

cmds=( netnat_test netfilter_test cat\ /proc/net/status poweroff )
printf '%s\n' "${cmds[@]}" > "$out/guest.commands"

qemu=( qemu-system-riscv64 -machine virt -m 1G -nographic -smp "$smp"
       -bios default -global virtio-mmio.force-legacy=false
       -drive "file=$image,if=none,format=raw,id=x0"
       -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
       -netdev "user,id=net,hostfwd=tcp:127.0.0.1:18081-10.0.2.15:18081"
       -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4
       -kernel "$kernel"
       -append "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=lwip" )
printf '%q ' "${qemu[@]}" > "$out/qemu.argv"
printf '\n' >> "$out/qemu.argv"

# The host probe must run while the guest is listening, so both sides start
# together and the probe retries until the guest installs its rule.
{
  sleep 10
  for c in "${cmds[@]}"; do printf '%s\n' "$c"; sleep 3; done
} | timeout "${NET_GUEST_TIMEOUT:-300}" "${qemu[@]}" > "$out/guest.log" 2>&1 &
qemu_pid=$!
python3 tools/netnat_host_probe.py 18081 16 > "$out/probe.log" 2>&1
probe=$?
record_exit host-probe "$probe"
wait $qemu_pid
qemu_status=$?
record_exit qemu "$qemu_status"

check() {
  local label=$1 pattern=$2
  if grep -qE "$pattern" "$out/guest.log"; then
    record_exit "$label" 0
    return 0
  fi
  record_exit "$label" 1
  return 1
}

status=0
check netnat 'NETNAT_TEST: PASS' || status=1
check netfilter 'NETFILTER_TEST: PASS' || status=1
check poweroff 'System is going down for power-off NOW' || status=1
check no-panic 'PANIC|Kernel panic|page fault|assertion failed|ASSERT FAIL|BUG:' && status=1
check no-fail '(: FAIL)' && status=1
[[ $probe -eq 0 ]] || status=1

grep -nE 'NETNAT_TEST|NETFILTER_TEST|netfilter:|conntrack:|nat ' "$out/guest.log" \
  > "$out/verdicts.txt" 2>&1
df -h / > "$out/disk.after.txt" 2>&1
sha256sum "$kernel" "$image" >> "$out/images.sha256" 2>&1
printf '%s\n' "$status" > "$out/overall.exit"
exit "$status"
