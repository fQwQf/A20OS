#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${A20_HANDLE_SLOT_TEST_BUILD:-$(mktemp -d /tmp/a20-handle-slot-host.XXXXXX)}"
cc_bin="${CC:-cc}"
rustc_bin="${RUSTC:-rustc}"
mkdir -p "$build_root/include/core" "$build_root/c" "$build_root/rust"

cat > "$build_root/include/core/lock.h" <<'EOF'
#ifndef _CORE_LOCK_H
#define _CORE_LOCK_H
#include <pthread.h>
#include <stdint.h>
typedef struct { pthread_mutex_t mutex; } spinlock_t;
#define SPINLOCK_INIT { PTHREAD_MUTEX_INITIALIZER }
static inline void spin_init(spinlock_t *lock) { pthread_mutex_init(&lock->mutex, 0); }
static inline void spin_set_debug(spinlock_t *lock, const char *name, void *container) { (void)lock; (void)name; (void)container; }
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) { pthread_mutex_lock(&lock->mutex); return 0; }
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) { (void)flags; pthread_mutex_unlock(&lock->mutex); }
#endif
EOF

common_flags=(-std=gnu11 -O2 -ffunction-sections -fdata-sections -fno-builtin
  -fsanitize=${HOST_SANITIZERS:-address,undefined} -fno-omit-frame-pointer
  -DA20_HANDLE_SLOT_HOST_TEST -DCONFIG_ABI_NATIVE -DCONFIG_X86_64
  -DCONFIG_NATIVE_TASK_HANDLES_ONLY
  -I"$build_root/include" -I"$repo_root/kernel/arch/x86_64/include"
  -I"$repo_root/kernel/include" -I"$repo_root/kernel")

for impl in c rust; do
  cflags=("${common_flags[@]}")
  if [[ "$impl" == rust ]]; then
    cflags+=(-DCONFIG_HANDLE_SLOT_TRANSITIONS_RUST)
    "$rustc_bin" --edition=2021 --crate-type lib --emit=obj \
      --target x86_64-unknown-linux-gnu -C panic=abort -C opt-level=2 \
      "$repo_root/kernel/abi/native/handle_slot.rs" -o "$build_root/rust/handle_slot.o"
  fi
  "$cc_bin" "${cflags[@]}" -c "$repo_root/kernel/abi/native/handle_table.c" \
    -o "$build_root/c/handle_table-$impl.o"
  "$cc_bin" "${common_flags[@]}" -c "$repo_root/tools/test_handle_slot_host.c" \
    -o "$build_root/c/host-test.o"
  link_inputs=("$build_root/c/host-test.o" "$build_root/c/handle_table-$impl.o")
  if [[ "$impl" == rust ]]; then
    link_inputs+=("$build_root/rust/handle_slot.o")
  fi
  "$cc_bin" -pthread -fsanitize=${HOST_SANITIZERS:-address,undefined} \
    -Wl,--gc-sections "${link_inputs[@]}" -o "$build_root/c/test-$impl"
  "$build_root/c/test-$impl"
done

if [[ -z "${A20_HANDLE_SLOT_TEST_BUILD:-}" ]]; then
  rm -rf "$build_root"
fi
