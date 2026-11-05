#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_root="${A20_PARK_WAIT_TEST_BUILD:-$(mktemp -d /tmp/a20-park-wait-host.XXXXXX)}"
cc_bin="${CC:-cc}"
rustc_bin="${RUSTC:-rustc}"
sanitizers="${HOST_SANITIZERS:-address,undefined}"
mkdir -p "$build_root"

common_flags=(-std=c11 -O2 -Wall -Wextra -Werror -pthread
  -fsanitize="$sanitizers" -fno-omit-frame-pointer
  -I"$repo_root/kernel/include" -I"$repo_root/kernel")

for impl in c rust; do
  impl_flags=()
  link_inputs=()
  if [[ "$impl" == rust ]]; then
    impl_flags+=(-DTEST_PARK_WAIT_RUST)
    "$rustc_bin" --edition=2021 --crate-type lib --emit=obj \
      --target x86_64-unknown-linux-gnu -C panic=abort -C opt-level=2 \
      "$repo_root/kernel/proc/park_wait_state.rs" \
      -o "$build_root/park_wait_state-rust.o"
    link_inputs+=("$build_root/park_wait_state-rust.o")
  else
    "$cc_bin" "${common_flags[@]}" -c \
      "$repo_root/kernel/proc/park_wait_state.c" \
      -o "$build_root/park_wait_state-c.o"
    link_inputs+=("$build_root/park_wait_state-c.o")
  fi
  "$cc_bin" "${common_flags[@]}" "${impl_flags[@]}" \
    "$repo_root/tools/host-tests/test_park_wait_host.c" "${link_inputs[@]}" \
    -o "$build_root/test-$impl"
  "$build_root/test-$impl"
done

if [[ -z "${A20_PARK_WAIT_TEST_BUILD:-}" ]]; then
  rm -rf "$build_root"
fi
