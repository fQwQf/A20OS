#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${A20_CHANNEL_RECV_TXN_BUILD_DIR:-}" ]]; then
    OUT="$A20_CHANNEL_RECV_TXN_BUILD_DIR"
    mkdir -p "$OUT"
    KEEP=1
else
    OUT="$(mktemp -d "${TMPDIR:-/tmp}/a20-channel-recv-txn.XXXXXX")"
    KEEP="${A20_CHANNEL_RECV_TXN_KEEP_BUILD:-0}"
fi
cleanup() {
    if [[ "$KEEP" != 1 ]]; then rm -rf "$OUT"; fi
}
trap cleanup EXIT

cd "$ROOT"
rustc --edition=2021 --crate-type lib --emit=obj \
    --target x86_64-unknown-linux-gnu -C panic=abort -C opt-level=2 \
    -C codegen-units=1 kernel/abi/native/channel_recv_txn.rs \
    -o "$OUT/channel_recv_txn-rust.o"
cc -std=gnu11 -O2 -Wall -Wextra -Werror \
    -DA20_CHANNEL_RECV_TXN_HOST_TEST -DCONFIG_CHANNEL_RECV_TXN_RUST \
    -Ikernel/abi/native \
    tools/test-channel-recv-txn-host.c \
    kernel/abi/native/channel_recv_txn.c \
    "$OUT/channel_recv_txn-rust.o" -o "$OUT/test-channel-recv-txn-host"
"$OUT/test-channel-recv-txn-host"
