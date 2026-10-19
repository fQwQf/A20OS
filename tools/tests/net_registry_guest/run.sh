#!/bin/bash
# RV64 SMP2 guest evidence run for the socket registry / reference-ledger
# migration.
#
# Boots the production kernel built with NET_SOCKET_REGISTRY_IMPL=rust on two
# RV64 harts and drives the network suite plus the socket/SCM stress tests
# through the real syscall paths, then reads /proc/net/status so the reference
# ledger the Rust state machine owns is checked on a live system (a leaked
# socket or a double free would show up as live != 0 or faults != 0).
#
# Usage: run.sh EVIDENCE_DIR KERNEL_ELF FAT32_IMG [NR_CPUS] [COMMANDS_FILE]
#
# COMMANDS_FILE (optional) replaces the built-in guest command list, one command
# per line, so a single scenario can be driven on two kernels and compared.
# Resource rules: takes `activity` EX + `qemu-1` EX, one VM, <=1GiB.  Exit 75
# means the resource was busy.
set -uo pipefail

if [[ $# -lt 3 || $# -gt 5 ]]; then
  echo "usage: $0 EVIDENCE_DIR KERNEL_ELF FAT32_IMG [NR_CPUS] [COMMANDS_FILE]" >&2
  exit 64
fi
out=$1
kernel=$2
image=$3
smp=${4:-2}
commands_file=${5:-}
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

# Resource leases for an ordinary (non-perf) QEMU run: activity SHARED -- which
# coexists with peer builds -- plus the single global qemu-1 slot EXCLUSIVE.
exec 9>"$lockdir/activity.lock" || exit 1
flock -s 9
record_exit activity-shared 0
exec 8>"$lockdir/qemu-1.lock" || exit 1
# The single global QEMU slot is shared with five sibling sessions that often
# chain their runs back to back. A brief wait (not a queue) is enough to win the
# gap between two of their runs without holding anything else, and it stays
# inside the "short backoff" rule: past two seconds this run reports 75 and
# leaves the decision to its caller.
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
  echo "source_tree_sha256=$(git -C "$repo" rev-parse HEAD^{tree})"
} > "$out/source.txt" 2>&1
sha256sum "$kernel" "$image" > "$out/images.sha256" 2>&1
{
  qemu-system-riscv64 --version
  nproc
  uname -a
  lscpu 2>/dev/null | head -20
} > "$out/host.txt" 2>&1

if [[ -n "$commands_file" ]]; then
  mapfile -t cmds < "$commands_file"
  cp "$commands_file" "$out/guest.commands"
else
  cmds=(
    network_suite
    net_stress_test
    socket_stress
    scm_stress
    tcp_accept_test
    unix_test
    netopt_test
    cat\ /proc/net/status
    poweroff
  )
  printf '%s\n' "${cmds[@]}" > "$out/guest.commands"
fi

# NET_GUEST_TCPMODE selects the kernel's TCP path: "lwip" (the default, the
# stack path) or "fast" (the local pairing shortcut, which is what makes the
# connect-pairing transaction reachable from this runner).
tcpmode=${NET_GUEST_TCPMODE:-lwip}
qemu=( qemu-system-riscv64 -machine virt -m 1G -nographic -smp "$smp"
       -bios default -global virtio-mmio.force-legacy=false
       -drive "file=$image,if=none,format=raw,id=x0"
       -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
       -netdev user,id=net
       -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4
       -kernel "$kernel"
       -append "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=$tcpmode" )
printf 'tcpmode=%s\n' "$tcpmode" > "$out/tcpmode.txt"
printf '%q ' "${qemu[@]}" > "$out/qemu.argv"
printf '\n' >> "$out/qemu.argv"

# Feed the commands once the guest has finished booting; the log is captured
# verbatim so a FAIL or a panic cannot be hidden by the exit status.  The
# network suite alone runs a dozen sub-suites (netlink is 79 checks), so the
# wall-clock budget is generous: a short one would truncate the log and look
# like a hang.  NET_GUEST_TIMEOUT overrides it for a focused scenario, so one
# acquisition of the single global QEMU slot is as short as the scenario allows.
{
  sleep 10
  for c in "${cmds[@]}"; do printf '%s\n' "$c"; sleep 3; done
} | timeout "${NET_GUEST_TIMEOUT:-900}" "${qemu[@]}" > "$out/guest.log" 2>&1
qemu_status=$?
record_exit qemu "$qemu_status"

# `check` records 0 when the pattern is present and 1 when it is absent, so a
# positive check passes on 0 and a "must not appear" check passes on 1.
check() {
  local label=$1 pattern=$2
  if grep -qE "$pattern" "$out/guest.log"; then
    record_exit "$label" 0
    return 0
  fi
  record_exit "$label" 1
  return 1
}

# Gate on the verdict of every test the command list actually ran, plus the
# conditions that always apply. A test that was not requested is not gated, so a
# focused scenario can be driven on two kernels and compared on its own terms.
declare -A markers=(
  [network_suite]='NETWORK_SUITE: PASS'
  [net_stress_test]='NET_STRESS_TEST: PASS'
  [socket_stress]='SOCKET_STRESS: PASS'
  [scm_stress]='SCM_STRESS: PASS'
  [tcp_accept_test]='TCP_ACCEPT_TEST: PASS'
  [tcp_edge_test]='TCP_EDGE_TEST: PASS'
  [tcp_loopback_test]='TCP_LOOPBACK_TEST: PASS'
  [udp_loopback_test]='UDP_LOOPBACK_TEST: PASS'
  [unix_test]='UNIX_TEST: PASS'
  [netopt_test]='NETOPT: PASS'
  [packet_test]='PACKET_TEST: PASS'
  [packet_rx_test]='PACKET_RX_TEST: PASS'
  [netfilter_test]='NETFILTER_TEST: PASS'
  [netnat_test]='NETNAT_TEST: PASS'
)

status=0
for c in "${cmds[@]}"; do
  c=${c%% *}
  marker=${markers[$c]:-}
  [ -z "$marker" ] && continue
  check "test-$c" "$marker" || status=1
done
check poweroff 'System is going down for power-off NOW' || status=1
# A bare ASSERT matches the CONFIG_*_ASSERT=0 rows in /proc output, so the panic
# pattern names what an actual failure prints.
check no-panic 'PANIC|Kernel panic|page fault|assertion failed|ASSERT FAIL|BUG:' && status=1
check no-fail '(: FAIL)' && status=1

# The ledger row the Rust state machine owns.  A double free or a release of a
# non-socket is what faults counts; live is reported as-is rather than forced to
# zero, because a test that leaves a descriptor open is not a kernel leak.
check ref-ledger-no-faults 'net_sock_ref: .*faults=0' || status=1
if grep -qE 'net_sock_ref: live=0 ' "$out/guest.log"; then
  record_exit ref-ledger-live-zero 0
else
  record_exit ref-ledger-live-zero 1
fi
grep -nE 'net_sock_ref' "$out/guest.log" > "$out/ledger.txt" 2>&1

grep -nE 'net_sock_ref|NETWORK_SUITE|NET_STRESS_TEST|SOCKET_STRESS|SCM_STRESS|TCP_ACCEPT_TEST|UNIX_TEST|NETOPT' \
  "$out/guest.log" > "$out/verdicts.txt" 2>&1
df -h / > "$out/disk.after.txt" 2>&1
sha256sum "$kernel" "$image" >> "$out/images.sha256" 2>&1
printf '%s\n' "$status" > "$out/overall.exit"
exit "$status"
