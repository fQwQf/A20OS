#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/a20-driver-registry.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

rustc --edition=2021 --test \
    "$repo/kernel/drivers/core/registry_state.rs" \
    -o "$tmp/driver-registry-state-test"
"$tmp/driver-registry-state-test"
