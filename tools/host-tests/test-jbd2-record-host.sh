#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bin=$(mktemp "${TMPDIR:-/tmp}/a20-jbd2-record.XXXXXX")
trap 'rm -f "$bin"' EXIT HUP INT TERM
rustc --test --edition=2021 "$root/user/svc/jbd2_record.rs" -o "$bin"
"$bin"
