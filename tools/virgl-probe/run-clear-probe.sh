#!/usr/bin/env bash
# Build and run tools/virgl-probe/probe-clear-stream.c against the locally built
# virglrenderer.
#
# This is the fast loop for "the guest accepted the command stream and nothing
# rendered".  Through QEMU that question costs a multi-minute boot per attempt
# and the guest can only answer "the host took the bytes"; this runs the same
# virglrenderer calls in-process, so vrend's own diagnostics are visible and an
# experiment takes seconds.
#
# It must run under tools/with-virgl-display.sh's environment (Mesa as the EGL
# vendor, the proprietary GPU's node hidden), which is what the wrapper does.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/tools/virgl/src/virglrenderer-virglrenderer-1.3.0/src"
GEN="$ROOT/tools/virgl/build/src"
PREFIX="${VIRGL_PREFIX:-$ROOT/tools/virgl/install}"
LIBDIR=""
for d in "$PREFIX"/lib/*-linux-gnu "$PREFIX"/lib; do
    [[ -e "$d/libvirglrenderer.so.1" ]] && { LIBDIR="$d"; break; }
done
if [[ -z "$LIBDIR" ]]; then
    echo "error: no libvirglrenderer under $PREFIX; run tools/build-virglrenderer.sh build" >&2
    exit 1
fi
if [[ ! -f "$GEN/virgl-version.h" ]]; then
    echo "error: $GEN/virgl-version.h missing; the meson build directory is stale" >&2
    exit 1
fi

OUT="${TMPDIR:-/tmp}/a20-virglprobe"
gcc -std=gnu11 -O1 -g -o "$OUT" "$ROOT/tools/virgl-probe/probe-clear-stream.c" \
    -I"$SRC" -I"$SRC/vrend" -I"$GEN" -L"$LIBDIR" -lvirglrenderer -lEGL
LD_LIBRARY_PATH="$LIBDIR:${LD_LIBRARY_PATH:-}" exec "$OUT"