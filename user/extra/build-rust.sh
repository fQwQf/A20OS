#!/usr/bin/env bash
set -euo pipefail

# Install the official RISC-V Rust distribution needed by the A20OS extra
# image.  rustc/cargo/rustfmt run against glibc; generated programs use the
# bundled static musl target and the native GCC package as their linker.
#
# Usage: build-rust.sh <install-dir>

RUST_VERSION=1.96.0
RUST_DATE=2026-05-28
HOST_TRIPLE=riscv64gc-unknown-linux-gnu
MUSL_TRIPLE=riscv64gc-unknown-linux-musl
RUST_SHA256=74902bc33de609a6ac89782fa987f0e35b08bf32cc5e838eb7c542a18c7ec554
MUSL_STD_SHA256=c65e85287b948cd5f3a2c8a18f51d44a9768829d9ee13d99e81953eee440f500

INSTALL_DIR="${1:?Usage: $0 <install-dir>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="$USER_DIR/external/rust/dist"
RUST_ARCHIVE="$DIST_DIR/rust-$RUST_VERSION-$HOST_TRIPLE.tar.xz"
MUSL_STD_ARCHIVE="$DIST_DIR/rust-std-$RUST_VERSION-$MUSL_TRIPLE.tar.xz"
RUST_URL="https://static.rust-lang.org/dist/$RUST_DATE/$(basename "$RUST_ARCHIVE")"
MUSL_STD_URL="https://static.rust-lang.org/dist/$RUST_DATE/$(basename "$MUSL_STD_ARCHIVE")"
WORK_DIR="$(dirname "$INSTALL_DIR")/rust-work"

download() {
    local url="$1" archive="$2" checksum="$3"

    if [ ! -f "$archive" ]; then
        mkdir -p "$DIST_DIR"
        echo "[RUST] Downloading $(basename "$archive")..."
        curl --fail --location --retry 3 --output "$archive.tmp" "$url"
        mv "$archive.tmp" "$archive"
    fi

    printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
}

download "$RUST_URL" "$RUST_ARCHIVE" "$RUST_SHA256"
download "$MUSL_STD_URL" "$MUSL_STD_ARCHIVE" "$MUSL_STD_SHA256"

if [ -f "$INSTALL_DIR/.rust-version" ] && \
   [ "$(cat "$INSTALL_DIR/.rust-version")" = "$RUST_VERSION" ] && \
   [ -x "$INSTALL_DIR/bin/rustc" ] && \
   [ -x "$INSTALL_DIR/bin/cargo" ] && \
   [ -x "$INSTALL_DIR/bin/rustfmt" ] && \
   [ -x "$INSTALL_DIR/bin/cargo-fmt" ]; then
    echo "[RUST] Rust $RUST_VERSION already installed in $INSTALL_DIR"
    exit 0
fi

rm -rf "$WORK_DIR" "$INSTALL_DIR"
mkdir -p "$WORK_DIR" "$INSTALL_DIR"
tar -xJf "$RUST_ARCHIVE" -C "$WORK_DIR"
tar -xJf "$MUSL_STD_ARCHIVE" -C "$WORK_DIR"

RUST_SRC="$WORK_DIR/rust-$RUST_VERSION-$HOST_TRIPLE"
MUSL_STD_SRC="$WORK_DIR/rust-std-$RUST_VERSION-$MUSL_TRIPLE"

echo "[RUST] Installing rustc, cargo, rustfmt, and $HOST_TRIPLE std..."
"$RUST_SRC/install.sh" \
    --prefix="$INSTALL_DIR" \
    --components=rustc,cargo,rustfmt-preview,rust-std-$HOST_TRIPLE \
    --disable-ldconfig

echo "[RUST] Installing $MUSL_TRIPLE std..."
"$MUSL_STD_SRC/install.sh" \
    --prefix="$INSTALL_DIR" \
    --components=rust-std-$MUSL_TRIPLE \
    --disable-ldconfig

printf '%s\n' "$RUST_VERSION" > "$INSTALL_DIR/.rust-version"
rm -rf "$WORK_DIR"
echo "[RUST] Installed Rust $RUST_VERSION in $INSTALL_DIR"
