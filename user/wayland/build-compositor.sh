#!/usr/bin/env bash
# Build the labwc Wayland compositor (wlroots-based) for A20OS.
#
# labwc is XFCE's reference Wayland compositor: it natively implements the
# wlr-layer-shell / foreign-toplevel / output-power-management protocols
# that the XFCE 4.20 components (panel, xfdesktop) need, which Weston does
# not provide.
#
# Usage: user/wayland/build-compositor.sh [ARCH] [phase]
#   ARCH:  riscv64 (default)
#   phase: wlroots | labwc   (no phase = build both in dependency order)
#
# Dependencies already in the sysroot: libdrm >= 2.4.122, wayland-server,
# wayland-protocols >= 1.41, libinput, libxkbcommon, pixman, libseat,
# hwdata (host), Mesa EGL/GLES (gbm + gles2 renderer).

set -euo pipefail

ARCH=${1:-riscv64}
PHASE=${2:-}
USER_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WL_DIR=$USER_DIR/wayland
BUILD=$USER_DIR/build/wayland
B=$BUILD/$ARCH
SYSROOT=$B/sysroot
MUSL_TARGET=$ARCH
case "$ARCH" in
    riscv64) MUSL_TARGET=riscv64 ;;
    loongarch64) MUSL_TARGET=loongarch64 ;;
    aarch64) MUSL_TARGET=aarch64 ;;
    x86_64) MUSL_TARGET=x86_64 ;;
esac

export PATH=$HOME/.local/bin:$HOME/.local/rootfs/usr/bin:$PATH
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=

TOOLCHAIN=$BUILD/toolchain/$MUSL_TARGET-linux-musl-cross
CC=$TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-gcc
CXX=$TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-g++
AR=$TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-ar
STRIP=$TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-strip

cat > /tmp/wlr-cross-$ARCH.ini <<EOF
[host_machine]
system = 'linux'
cpu_family = '$ARCH'
cpu = '$ARCH'
endian = 'little'

[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkgconfig = 'pkg-config'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$SYSROOT/lib/pkgconfig', '$SYSROOT/share/pkgconfig']

[built-in options]
c_args = ['-O2', '-D_GNU_SOURCE', '-fPIC', '-I$SYSROOT/include', '-I$SYSROOT/include/libdrm']
c_link_args = ['-fPIC', '-L$SYSROOT/lib', '-Wl,-rpath-link,$SYSROOT/lib']
cpp_args = ['-O2', '-D_GNU_SOURCE', '-fPIC', '-I$SYSROOT/include', '-I$SYSROOT/include/libdrm']
cpp_link_args = ['-fPIC', '-L$SYSROOT/lib', '-Wl,-rpath-link,$SYSROOT/lib']
EOF

# Host needs hwdata's pnp.ids for libdisplay-info to generate its table.
if [ ! -f /usr/share/hwdata/pnp.ids ]; then
    echo "build-compositor: /usr/share/hwdata/pnp.ids missing on host" >&2
    exit 1
fi

# ---------------------------------------------------------- libdisplay-info
if [ -z "$PHASE" ] || [ "$PHASE" = "libdisplay-info" ]; then
    echo "=== libdisplay-info ==="
    OB=$B/build-libdisplay-info
    rm -rf "$OB"
    meson setup "$OB" "$USER_DIR/external/gui/libdisplay-info" \
        --cross-file /tmp/wlr-cross-$ARCH.ini \
        --prefix="$SYSROOT" --libdir=lib
    ninja -C "$OB"
    ninja -C "$OB" install
fi

# ----------------------------------------------------------------- wlroots
if [ -z "$PHASE" ] || [ "$PHASE" = "wlroots" ]; then
    echo "=== wlroots ==="
    OB=$B/build-wlroots
    rm -rf "$OB"
    meson setup "$OB" "$USER_DIR/external/gui/wlroots" \
        --cross-file /tmp/wlr-cross-$ARCH.ini \
        --prefix="$SYSROOT" --libdir=lib \
        -Dexamples=false -Dxwayland=disabled \
        -Drenderers=gles2 -Dbackends=drm,libinput -Dallocators=gbm
    ninja -C "$OB"
    ninja -C "$OB" install
fi

# ------------------------------------------------------------------ labwc
if [ -z "$PHASE" ] || [ "$PHASE" = "labwc" ]; then
    echo "=== labwc ==="
    OB=$B/build-labwc
    rm -rf "$OB"
    meson setup "$OB" "$USER_DIR/external/gui/labwc" \
        --cross-file /tmp/wlr-cross-$ARCH.ini \
        --prefix="$SYSROOT" --libdir=lib \
        -Dxwayland=disabled
    ninja -C "$OB"
    ninja -C "$OB" install
fi

echo "=== done ==="
