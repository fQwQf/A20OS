#!/usr/bin/env bash
# Build the Wayland/Weston userspace stack for A20OS.
#
# Usage: user/wayland/build.sh [ARCH] [phase ...]
#   ARCH: riscv64 (default)
#   phase: musl wayland-native wayland protocols pixman xkbcommon libevdev
#          stubs libdrm libinput weston image
#   No phase = build everything in dependency order.
#
# Layout:
#   user/build/wayland/<arch>/sysroot   target sysroot (lib, include, share)
#   user/build/wayland/<arch>/build-*   per-package build dirs
#   user/build/wayland/host/            native wayland-scanner toolchain

set -euo pipefail

ARCH=${1:-riscv64}
shift || true
PHASES=("$@")

USER_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WL_DIR=$USER_DIR/wayland
BUILD=$USER_DIR/build/wayland
ABUILD=$BUILD/$ARCH
SYSROOT=$ABUILD/sysroot
B=$ABUILD
MUSL_BASE=$USER_DIR/external/musl
MUSL_SH=$BUILD/musl-$ARCH
HOST_TOOLS=$BUILD/host

CROSS_riscv64=riscv64-linux-gnu-
ARCH_CFLAGS_riscv64="-mcmodel=medany -march=rv64g -mabi=lp64d"
CPU_FAMILY_riscv64=riscv64
MUSL_TARGET_riscv64=riscv64

CROSS_VAR=CROSS_$ARCH
CROSS=${CROSS:-${!CROSS_VAR}}
CC=${CROSS}gcc
AR=${CROSS}ar
ARCH_CFLAGS_VAR=ARCH_CFLAGS_$ARCH
ARCH_CFLAGS=${!ARCH_CFLAGS_VAR}
CPU_FAMILY_VAR=CPU_FAMILY_$ARCH
CPU_FAMILY=${!CPU_FAMILY_VAR}
MUSL_TARGET_VAR=MUSL_TARGET_$ARCH
MUSL_TARGET=${!MUSL_TARGET_VAR}
DYNLINKER=/lib/ld-musl-$MUSL_TARGET.so.1

export PATH=$HOME/.local/bin:$HOME/.local/rootfs/usr/bin:$PATH
export BISON_PKGDATADIR=$HOME/.local/rootfs/usr/share/bison
export M4=$HOME/.local/rootfs/usr/bin/m4

WANT_ALL=0
if [ ${#PHASES[@]} -eq 0 ]; then
    WANT_ALL=1
    PHASES=(musl wayland-native libffi wayland protocols pixman xkbcommon \
            libevdev stubs libdrm libinput weston)
fi

want() {
    [ $WANT_ALL -eq 1 ] && return 0
    local p
    for p in "${PHASES[@]}"; do [ "$p" = "$1" ] && return 0; done
    return 1
}

stamp() { [ -f "$B/stamp/$1" ]; }
mark() { mkdir -p "$B/stamp" && touch "$B/stamp/$1"; }

MUSL_INC=(
    -isystem "$MUSL_SH/obj/include"
    -isystem "$MUSL_BASE/include"
    -isystem "$MUSL_BASE/arch/$ARCH"
    -isystem "$MUSL_BASE/arch/generic"
)

# ---------------------------------------------------------------- musl (shared)
if want musl && ! stamp musl; then
    echo "=== musl (shared) ==="
    rm -rf "$MUSL_SH"
    mkdir -p "$MUSL_SH"
    (cd "$MUSL_SH" && env -u ARCH -u MAKEFLAGS sh "$MUSL_BASE/configure" \
        --target="$MUSL_TARGET" \
        --prefix="$MUSL_SH/install" \
        CC="$CC" CROSS_COMPILE="$CROSS" \
        CFLAGS="-O2 $ARCH_CFLAGS")
    env -u ARCH make -C "$MUSL_SH" -j"$(nproc)"
    mark musl
fi

# musl-gcc wrapper (specs-based) as the meson cross compiler
MUSL_GCC=$MUSL_SH/musl-gcc-a20
cat > "$MUSL_GCC" <<EOF
#!/bin/sh
exec $CC -specs "$MUSL_SH/lib/musl-gcc.specs" $ARCH_CFLAGS "\$@"
EOF
chmod +x "$MUSL_GCC"

meson_cross_ini() {
    cat > "$B/meson-cross.ini" <<EOF
[host_machine]
system = 'linux'
cpu_family = '$CPU_FAMILY'
cpu = '$ARCH'
endian = 'little'

[binaries]
c = '$MUSL_GCC'
ar = '$AR'
strip = '${CROSS}strip'
pkgconfig = 'pkg-config'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$SYSROOT/lib/pkgconfig', '$SYSROOT/share/pkgconfig']

[built-in options]
c_args = ['-O2', '-D_GNU_SOURCE', '-fPIC', '-I$SYSROOT/include', '-I$SYSROOT/include/libdrm']
c_link_args = ['-fPIC', '-L$SYSROOT/lib']
default_library = 'both'
EOF
}

HOST_PC=""
for d in "$HOST_TOOLS"/wayland/lib/pkgconfig "$HOST_TOOLS"/wayland/lib/*/pkgconfig; do
    [ -d "$d" ] && HOST_PC="${HOST_PC:+$HOST_PC:}$d"
done
PKG_ENV=(env "PKG_CONFIG_LIBDIR=$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig" \
            "PKG_CONFIG_PATH=" \
            "PKG_CONFIG_SYSROOT_DIR=")

# ------------------------------------------------- native wayland-scanner
if want wayland-native && ! stamp wayland-native; then
    echo "=== wayland (native scanner) ==="
    rm -rf "$B/build-wayland-native"
    meson setup "$B/build-wayland-native" "$USER_DIR/external/wayland" \
        --prefix="$HOST_TOOLS/wayland" \
        -Ddocumentation=false -Dtests=false -Ddtd_validation=false \
        --default-library=static
    ninja -C "$B/build-wayland-native"
    ninja -C "$B/build-wayland-native" install
    # Meson's build-machine pkg-config lookup uses the cross
    # PKG_CONFIG_LIBDIR; make the native scanner visible there too.
    mkdir -p "$SYSROOT/lib/pkgconfig"
    for d in "$HOST_TOOLS"/wayland/lib/pkgconfig "$HOST_TOOLS"/wayland/lib/*/pkgconfig; do
        [ -f "$d/wayland-scanner.pc" ] && \
            cp "$d/wayland-scanner.pc" "$SYSROOT/lib/pkgconfig/"
    done
    mark wayland-native
fi

meson_cross_ini

meson_pkg() {
    # meson_pkg <name> <srcdir> <meson args...>
    local name=$1 src=$2
    shift 2
    rm -rf "$B/build-$name"
    "${PKG_ENV[@]}" meson setup "$B/build-$name" "$src" \
        --cross-file "$B/meson-cross.ini" \
        --prefix="$SYSROOT" \
        --libdir=lib \
        "$@"
    ninja -C "$B/build-$name"
    ninja -C "$B/build-$name" install
}

# ---------------------------------------------------------------- libffi
if want libffi && ! stamp libffi; then
    echo "=== libffi ==="
    SRC=$BUILD/libffi-3.4.6
    OB=$B/build-libffi
    rm -rf "$OB" && mkdir -p "$OB"
    (cd "$OB" && "$SRC/configure" \
        --host="$MUSL_TARGET-linux-gnu" \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --includedir="$SYSROOT/include" \
        --disable-static --disable-docs \
        CC="$MUSL_GCC" CFLAGS="-O2 -fPIC")
    make -C "$OB" -j"$(nproc)"
    make -C "$OB" install
    # libffi installs headers into lib/libffi-3.4.6/include; move them out
    if [ -d "$SYSROOT/lib/libffi-3.4.6/include" ]; then
        cp "$SYSROOT/lib/libffi-3.4.6/include/"*.h "$SYSROOT/include/"
        rm -rf "$SYSROOT/lib/libffi-3.4.6"
    fi
    sed -i -e 's|^includedir=.*|includedir='"$SYSROOT"'/include|' \
        -e 's|^toolexeclibdir=.*|toolexeclibdir='"$SYSROOT"'/lib|' \
        "$SYSROOT/lib/pkgconfig/libffi.pc" 2>/dev/null || true
    mark libffi
fi

# ---------------------------------------------------------------- wayland
if want wayland && ! stamp wayland; then
    echo "=== wayland (target) ==="
    meson_pkg wayland "$USER_DIR/external/wayland" \
        -Ddocumentation=false -Dtests=false -Ddtd_validation=false \
        -Dscanner=false
    mark wayland
fi

# ------------------------------------------------------- wayland-protocols
if want protocols && ! stamp protocols; then
    echo "=== wayland-protocols ==="
    meson_pkg wayland-protocols "$USER_DIR/external/wayland-protocols" \
        -Dtests=false
    mark protocols
fi

# ---------------------------------------------------------------- pixman
if want pixman && ! stamp pixman; then
    echo "=== pixman ==="
    meson_pkg pixman "$USER_DIR/external/pixman" \
        -Dtests=disabled -Ddemos=disabled -Dgtk=disabled \
        -Dlibpng=disabled -Dgnuplot=false -Dopenmp=disabled
    mark pixman
fi

# ------------------------------------------------------------ libxkbcommon
if want xkbcommon && ! stamp xkbcommon; then
    echo "=== libxkbcommon ==="
    meson_pkg libxkbcommon "$USER_DIR/external/libxkbcommon" \
        -Denable-x11=false -Denable-wayland=false -Denable-tools=false \
        -Denable-docs=false -Denable-bash-completion=false \
        -Denable-xkbregistry=false \
        -Dxkb-config-root=/usr/share/X11/xkb \
        -Dx-locale-root=/usr/share/X11/locale
    mark xkbcommon
fi

# --------------------------------------------------------------- libevdev
if want libevdev && ! stamp libevdev; then
    echo "=== libevdev (manual) ==="
    # linux/input.h UAPI headers (musl does not ship them)
    mkdir -p "$SYSROOT/include/linux"
    for h in input.h input-event-codes.h uinput.h; do
        [ -f "/usr/$MUSL_TARGET-linux-gnu/include/linux/$h" ] && \
            cp "/usr/$MUSL_TARGET-linux-gnu/include/linux/$h" \
               "$SYSROOT/include/linux/"
    done
    SRC=$USER_DIR/external/libevdev
    OB=$B/build-libevdev
    rm -rf "$OB" && mkdir -p "$OB"
    cat > "$OB/config.h" <<'EOF'
#define HAVE_CLOCK_GETTIME 1
#define HAVE_STRNDUP 1
#define HAVE_STRNDUPA 0
#define HAVE_MALLOC_INFO 0
#define HAVE_OPEN64 0
#define HAVE_LSTAT64 0
#define HAVE_FSTAT64 0
#define HAVE_STAT64 0
#define HAVE_FSTATAT64 0
#define HAVE_MMAP64 0
#define HAVE_IOCTL 1
#define HAVE_GETPAGESIZE 1
#define HAVE_SYSCALL 1
#define HAVE_FCNTL 1
#define HAVE_EPOLL 1
#define HAVE_PIPE2 1
#define HAVE_EVENTFD 1
#define HAVE_MEMFD_CREATE 1
#define HAVE_CC 1
#define _GNU_SOURCE 1
EOF
    python3 "$SRC/libevdev/make-event-names.py" \
        "$SRC/include/linux/linux/input.h" \
        "$SRC/include/linux/linux/input-event-codes.h" \
        > "$OB/event-names.h"
    for f in libevdev libevdev-names libevdev-uinput; do
        "$MUSL_GCC" -O2 -D_GNU_SOURCE -fPIC \
            -I"$OB" -I"$SRC" -I"$SRC/libevdev" -I"$SRC/include" \
            -I"$SYSROOT/include" \
            -c "$SRC/libevdev/$f.c" -o "$OB/$f.o"
    done
    "$MUSL_GCC" -shared -Wl,-soname,libevdev.so.2 \
        -o "$OB/libevdev.so.2.3.0" \
        "$OB/libevdev.o" "$OB/libevdev-names.o" "$OB/libevdev-uinput.o"
    mkdir -p "$SYSROOT/lib" "$SYSROOT/include/libevdev-1.0/libevdev" \
             "$SYSROOT/lib/pkgconfig"
    cp "$OB/libevdev.so.2.3.0" "$SYSROOT/lib/"
    ln -sf libevdev.so.2.3.0 "$SYSROOT/lib/libevdev.so.2"
    ln -sf libevdev.so.2.3.0 "$SYSROOT/lib/libevdev.so"
    cp "$SRC/libevdev/libevdev.h" "$SRC/libevdev/libevdev-uinput.h" \
        "$SYSROOT/include/libevdev-1.0/libevdev/"
    sed -e "s|^prefix=.*|prefix=$SYSROOT|" \
        -e "s|^libdir=.*|libdir=$SYSROOT/lib|" \
        -e "s|^includedir=.*|includedir=$SYSROOT/include|" \
        "$SRC/libevdev.pc.in" \
        -e 's|@VERSION@|1.13.3|' \
        -e 's|@LIBEVDEV_PC_LIBS_PRIVATE@||' \
        > "$SYSROOT/lib/pkgconfig/libevdev.pc"
    mark libevdev
fi

# ---------------------------------------------------- udev / mtdev stubs
if want stubs && ! stamp stubs; then
    echo "=== udev/mtdev stubs ==="
    OB=$B/build-stubs
    rm -rf "$OB" && mkdir -p "$OB"
    "$MUSL_GCC" -O2 -fPIC -I"$WL_DIR/stub" -I"$SYSROOT/include" \
        -c "$WL_DIR/stub/udev.c" -o "$OB/udev.o"
    "$MUSL_GCC" -shared -Wl,-soname,libudev.so.1 \
        -o "$OB/libudev.so.1.7.99" "$OB/udev.o"
    cp "$OB/libudev.so.1.7.99" "$SYSROOT/lib/"
    ln -sf libudev.so.1.7.99 "$SYSROOT/lib/libudev.so.1"
    ln -sf libudev.so.1.7.99 "$SYSROOT/lib/libudev.so"
    cp "$WL_DIR/stub/udev.h" "$SYSROOT/include/libudev.h"
    sed -i 's|#include "udev.h"|#include <libudev.h>|' "$SYSROOT/include/libudev.h"
    cat > "$SYSROOT/lib/pkgconfig/libudev.pc" <<EOF
prefix=$SYSROOT
libdir=$SYSROOT/lib
includedir=$SYSROOT/include
Name: libudev
Description: udev stub for A20OS
Version: 243
Libs: -L\${libdir} -ludev
Cflags: -I\${includedir}
EOF
    "$MUSL_GCC" -O2 -fPIC -I"$WL_DIR/stub" -I"$SYSROOT/include" \
        -c "$WL_DIR/stub/mtdev.c" -o "$OB/mtdev.o"
    "$MUSL_GCC" -shared -Wl,-soname,libmtdev.so.1 \
        -o "$OB/libmtdev.so.1.1.6" "$OB/mtdev.o"
    cp "$OB/libmtdev.so.1.1.6" "$SYSROOT/lib/"
    ln -sf libmtdev.so.1.1.6 "$SYSROOT/lib/libmtdev.so.1"
    ln -sf libmtdev.so.1.1.6 "$SYSROOT/lib/libmtdev.so"
    cp "$WL_DIR/stub/mtdev.h" "$SYSROOT/include/mtdev-plumbing.h"
    sed -i 's|#include "mtdev.h"|#include <mtdev-plumbing.h>|' "$SYSROOT/include/mtdev-plumbing.h"
    cat > "$SYSROOT/lib/pkgconfig/mtdev.pc" <<EOF
prefix=$SYSROOT
libdir=$SYSROOT/lib
includedir=$SYSROOT/include
Name: mtdev
Description: mtdev stub for A20OS
Version: 1.1.6
Libs: -L\${libdir} -lmtdev
Cflags: -I\${includedir}
EOF
    mark stubs
fi

# -------------------------------------------- libdrm headers + stub pc
if want libdrm && ! stamp libdrm; then
    echo "=== libdrm headers ==="
    SRC=$USER_DIR/external/libdrm
    mkdir -p "$SYSROOT/include/libdrm" "$SYSROOT/include/drm"
    cp "$SRC/include/drm/"*.h "$SYSROOT/include/libdrm/" 2>/dev/null || true
    cp "$SRC/include/drm/"*.h "$SYSROOT/include/drm/" 2>/dev/null || true
    cat > "$SYSROOT/lib/pkgconfig/libdrm.pc" <<EOF
prefix=$SYSROOT
libdir=$SYSROOT/lib
includedir=$SYSROOT/include
Name: libdrm
Description: libdrm headers stub for A20OS
Version: 2.4.120
Libs: -L\${libdir} -ldrm
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF
    mark libdrm
fi

# --------------------------------------------------------------- libinput
if want libinput && ! stamp libinput; then
    echo "=== libinput ==="
    meson_pkg libinput "$USER_DIR/external/libinput" \
        -Dlibwacom=false -Ddebug-gui=false -Dtests=false \
        -Ddocumentation=false -Dinstall-tests=false \
        -Dzshcompletiondir=no
    mark libinput
fi

# ---------------------------------------------------------------- weston
if want weston && ! stamp weston; then
    echo "=== weston ==="
    for p in "$WL_DIR/patches/weston-"*.patch; do
        (cd "$USER_DIR/external/weston" && git apply --check "$p" 2>/dev/null && git apply "$p") || true
    done
    meson_pkg weston "$USER_DIR/external/weston" \
        -Dbackend-drm=false -Dbackend-headless=false -Dbackend-rdp=false \
        -Dbackend-wayland=false -Dbackend-x11=false -Dbackend-fbdev=true \
        -Dbackend-default=fbdev \
        -Drenderer-gl=false -Dscreenshare=false -Dxwayland=false \
        -Dsystemd=false -Dweston-launch=false -Dlauncher-logind=false \
        -Dremoting=false -Dpipewire=false \
        -Dshell-desktop=true -Dshell-fullscreen=false -Dshell-ivi=false \
        -Dcolor-management-lcms=false -Dcolor-management-colord=false \
        -Dimage-jpeg=false -Dimage-webp=false \
        -Da20-no-cairo=true \
        -Dsimple-clients=shm -Ddemo-clients=false -Dtools= \
        -Dwcap-decode=false -Dtest-junit-xml=false
    mark weston
fi

echo "=== done ==="
