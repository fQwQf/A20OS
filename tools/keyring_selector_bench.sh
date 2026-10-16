#!/usr/bin/env bash
set -euo pipefail

root=$(git rev-parse --show-toplevel)
: "${KEYRING_C_KERNEL:?set KEYRING_C_KERNEL to the final-source C-selector kernel.}"
: "${KEYRING_RUST_KERNEL:?set KEYRING_RUST_KERNEL to the Rust-selector kernel.}"
: "${KEYRING_FAT32_IMG:?set KEYRING_FAT32_IMG to one shared image containing /bin/keyring_bench.}"
: "${KEYRING_BENCH_OUT:?set KEYRING_BENCH_OUT to a directory for raw logs and reports.}"
mkdir -p "$KEYRING_BENCH_OUT"
tmp_bin=$(mktemp)
trap 'rm "$tmp_bin"' EXIT

profiles=(c rust c rust c rust c rust c rust)
printf 'round,selector,add_update_ns,search_ns,read_ns,link_unlink_ns,log_sha256\n' \
  > "$KEYRING_BENCH_OUT/samples.csv"

for index in "${!profiles[@]}"; do
  round=$((index + 1))
  selector=${profiles[$index]}
  kernel=$KEYRING_C_KERNEL
  [[ $selector == rust ]] && kernel=$KEYRING_RUST_KERNEL
  log=$KEYRING_BENCH_OUT/round-$(printf '%02d' "$round")-$selector.qemu.log
  inputs=$KEYRING_BENCH_OUT/round-$(printf '%02d' "$round")-$selector.inputs.txt
  {
    printf 'round=%s selector=%s\n' "$round" "$selector"
    printf 'kernel='; sha256sum "$kernel"
    printf 'fat32='; sha256sum "$KEYRING_FAT32_IMG"
    mcopy -o -i "$KEYRING_FAT32_IMG" ::/keyring_bench "$tmp_bin"
    printf 'guest_binary='; sha256sum "$tmp_bin"
    printf 'source='; sha256sum "$root/user/cmds/core/keyring_bench.c"
  } > "$inputs"
  set +e
  (
    sleep 8
    printf '/bin/keyring_bench\n'
    sleep 1
    printf 'poweroff\n'
  ) | flock -s "$root/.git/parallel-rewrite/activity.lock" \
      flock -E 75 -n "$root/.git/parallel-rewrite/qemu-1.lock" \
      timeout 120s qemu-system-riscv64 \
        -machine virt -m 1G -nographic -monitor none -smp 2 -bios default \
        -global virtio-mmio.force-legacy=false \
        -drive "file=$KEYRING_FAT32_IMG,if=none,format=raw,id=x0" \
        -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
        -kernel "$kernel" > "$log" 2>&1
  qemu_rc=$?
  set -e
  printf 'qemu_exit=%s\n' "$qemu_rc" >> "$inputs"
  if [[ $qemu_rc -ne 0 ]]; then
    printf 'QEMU failed in round %s (%s), see %s\n' "$round" "$selector" "$log" >&2
    exit 1
  fi
  grep -q 'KEYRING_BENCH: PASS n=1000 warmup=100' "$log"
  grep -q 'System is going down for power-off NOW' "$log"
  line=$(grep 'KEYRING_BENCH: PASS n=1000 warmup=100' "$log")
  add=$(sed -n 's/.*add_update_ns=\([0-9][0-9]*\).*/\1/p' <<<"$line")
  search=$(sed -n 's/.*search_ns=\([0-9][0-9]*\).*/\1/p' <<<"$line")
  read_ns=$(sed -n 's/.*read_ns=\([0-9][0-9]*\).*/\1/p' <<<"$line")
  link=$(sed -n 's/.*link_unlink_ns=\([0-9][0-9]*\).*/\1/p' <<<"$line")
  log_sha=$(sha256sum "$log" | cut -d' ' -f1)
  printf '%s,%s,%s,%s,%s,%s,%s\n' "$round" "$selector" "$add" "$search" "$read_ns" "$link" "$log_sha" \
    >> "$KEYRING_BENCH_OUT/samples.csv"
done

printf 'P3_KEYRING_BENCH: PASS runs=10 alternating_pairs=5\n'
