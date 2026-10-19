#!/bin/bash
# Interleaved same-load C-vs-Rust guest measurement.
#
# Boots each kernel in turn with the same image, waits for the shell, times one
# workload from the moment the command is written to the moment the workload
# prints its verdict, and repeats in the order C,R,R,C,C,R,R,C so that drift in
# the host (cache state, background load, TCG warm-up) lands on both kernels
# instead of on whichever ran second.
#
# What is compared is the same binary on the same image with the same argv and
# the same console input; the only difference is the kernel.  The absolute
# numbers include guest bootless console latency and are reported as measured;
# the comparison is the per-round ratio, and every run records its source commit,
# kernel/image hashes and natural exits.
#
# Usage: run.sh EVIDENCE_DIR KERNEL_C KERNEL_R IMAGE_C IMAGE_R WORKLOAD [ROUNDS]
# Perf class: activity EX + qemu-1 EX, one VM, <=1 GiB.  Exit 75 = busy.
set -uo pipefail

if [[ $# -lt 6 || $# -gt 7 ]]; then
  echo "usage: $0 EVIDENCE_DIR KERNEL_C KERNEL_R IMAGE_C IMAGE_R WORKLOAD [ROUNDS]" >&2
  exit 64
fi
out=$1; kc=$2; kr=$3; ic=$4; ir=$5; workload=$6; rounds=${7:-8}
for p in "$out" "$kc" "$kr" "$ic" "$ir"; do
  [[ "$p" == /* ]] || { echo "usage: absolute paths" >&2; exit 64; }
done
mkdir -p "$(dirname -- "$out")"
if ! mkdir "$out"; then
  echo "evidence directory already exists: $out" >&2
  exit 73
fi

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir) || exit 74
lockdir="$common/parallel-rewrite"

record_exit() { printf '%s %s\n' "$1" "$2" >> "$out/exits.txt"; }
printf '%q ' "$0" "$@" > "$out/run.argv"; printf '\n' >> "$out/run.argv"

# Perf class: activity EXCLUSIVE + the single VM slot.
exec 9>"$lockdir/activity.lock" || exit 1
if ! flock -w 5 -x 9; then
  record_exit activity-exclusive 75
  exit 75
fi
record_exit activity-exclusive 0
exec 8>"$lockdir/qemu-1.lock" || exit 1
if ! flock -w 5 -x 8; then
  record_exit qemu-slot 75
  exit 75
fi
record_exit qemu-slot 0

{
  echo "source_commit=$(git -C "$repo" rev-parse HEAD)"
  echo "source_dirty_diff_sha256=$(git -C "$repo" diff | sha256sum | cut -d' ' -f1)"
  echo "workload=$workload"
  echo "rounds=$rounds"
} > "$out/source.txt" 2>&1
sha256sum "$kc" "$kr" "$ic" "$ir" > "$out/images.sha256" 2>&1

run_round() {  # tag kernel image
  local tag=$1 kernel=$2 image=$3
  local fifo="$out/.fifo-$tag"
  rm -f "$fifo"; mkfifo "$fifo"
  qemu-system-riscv64 -machine virt -m 1G -nographic -smp 2 \
    -bios default -global virtio-mmio.force-legacy=false \
    -drive "file=$image,if=none,format=raw,id=x0" \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -netdev user,id=net \
    -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
    -kernel "$kernel" \
    -append "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=lwip" \
    < "$fifo" > "$out/round-$tag.log" 2>&1 &
  local qp=$!
  exec 3>"$fifo"

  # Wait for the shell to print its prompt.
  local i
  for i in $(seq 1 200); do
    grep -q "^# " "$out/round-$tag.log" 2>/dev/null && break
    sleep 0.1
  done
  echo "echo PERF-READY" >&3
  for i in $(seq 1 200); do
    grep -q "PERF-READY" "$out/round-$tag.log" 2>/dev/null && break
    sleep 0.05
  done

  local t0 t1
  local marker
  case "$workload" in
    scm_stress) marker='^SCM_STRESS: (PASS|FAIL)' ;;
    socket_stress) marker='^SOCKET_STRESS: (PASS|FAIL)' ;;
    net_stress_test) marker='^NET_STRESS_TEST: (PASS|FAIL|SKIP)' ;;
    netopt_test) marker='^NETOPT: (PASS|FAIL)' ;;
    *) marker="^$(printf '%s' "$workload" | tr 'a-z' 'A-Z'): (PASS|FAIL)" ;;
  esac
  t0=$(date +%s%N)
  echo "$workload" >&3
  for i in $(seq 1 1200); do
    grep -qE "$marker" "$out/round-$tag.log" 2>/dev/null && break
    sleep 0.05
  done
  t1=$(date +%s%N)

  echo "poweroff" >&3
  exec 3>&-
  wait $qp
  local qrc=$?
  rm -f "$fifo"

  local elapsed_ms=$(( (t1 - t0) / 1000000 ))
  local verdict
  verdict=$(grep -E "$marker" "$out/round-$tag.log" | tail -1)
  printf '%s %s elapsed_ms=%s qemu_rc=%s verdict=%s\n' \
    "$tag" "$(basename "$kernel")" "$elapsed_ms" "$qrc" "$verdict" \
    | tee -a "$out/measurements.txt"
  record_exit "round-$tag" "$qrc"
}

order=()
for ((r = 0; r < rounds; r += 4)); do
  order+=( "c$r:-c" "r$((r+1)):-r" "r$((r+2)):-r" "c$((r+3)):-c" )
done
for entry in "${order[@]}"; do
  tag=${entry%%:*}; which=${entry##*:}
  if [[ "$which" == "-c" ]]; then
    run_round "$tag" "$kc" "$ic"
  else
    run_round "$tag" "$kr" "$ir"
  fi
done

# Medians over the plain elapsed_ms values, tagged by kernel.
awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^elapsed_ms=/) {
         v = $i; sub(/^elapsed_ms=/, "", v);
         if ($1 ~ /^c/) { c[nc++] = v } else { r[nr++] = v }
       } }
     END {
       for (i = 0; i < nc; i++) for (j = i+1; j < nc; j++)
         if (c[j] < c[i]) { t = c[i]; c[i] = c[j]; c[j] = t }
       for (i = 0; i < nr; i++) for (j = i+1; j < nr; j++)
         if (r[j] < r[i]) { t = r[i]; r[i] = r[j]; r[j] = t }
       printf "c_median_ms=%s r_median_ms=%s c_n=%d r_n=%d\n",
         nc ? c[int(nc/2)] : "na", nr ? r[int(nr/2)] : "na", nc, nr
     }' "$out/measurements.txt" | tee "$out/summary.txt"

df -h / > "$out/disk.after.txt" 2>&1
status=0
grep -q "c_median_ms=na" "$out/summary.txt" && status=1
grep -q "r_median_ms=na" "$out/summary.txt" && status=1
printf '%s\n' "$status" > "$out/overall.exit"
exit "$status"
