#!/usr/bin/env bash
# Build the Wayland/Weston userspace stack for A20OS.
#
# Usage: user/wayland/build.sh [ARCH] [phase ...]
#   ARCH: riscv64 (default)
#   phase: musl wayland-native wayland protocols pixman xkeyboard-config
#          xkbcommon libevdev
#          stubs libdrm libinput weston ffmpeg player
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

CROSS_loongarch64=loongarch64-linux-gnu-
ARCH_CFLAGS_loongarch64="-march=loongarch64 -mabi=lp64d"
CPU_FAMILY_loongarch64=loongarch64
MUSL_TARGET_loongarch64=loongarch64

CROSS_aarch64=aarch64-linux-gnu-
ARCH_CFLAGS_aarch64="-march=armv8-a"
CPU_FAMILY_aarch64=aarch64
MUSL_TARGET_aarch64=aarch64

CROSS_x86_64=x86_64-linux-gnu-
ARCH_CFLAGS_x86_64="-m64"
CPU_FAMILY_x86_64=x86_64
MUSL_TARGET_x86_64=x86_64
LIBFFI_HOST_x86_64=x86_64-linux-musl
UAPI_ASM_x86_64=/usr/include/x86_64-linux-gnu/asm

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
LIBFFI_HOST_VAR=LIBFFI_HOST_$ARCH
LIBFFI_HOST=${!LIBFFI_HOST_VAR:-$MUSL_TARGET-linux-gnu}
UAPI_ASM_VAR=UAPI_ASM_$ARCH
UAPI_ASM=${!UAPI_ASM_VAR:-/usr/$MUSL_TARGET-linux-gnu/include/asm}
DYNLINKER=/lib/ld-musl-$MUSL_TARGET.so.1

export PATH=$HOME/.local/bin:$HOME/.local/rootfs/usr/bin:$PATH
export BISON_PKGDATADIR=$HOME/.local/rootfs/usr/share/bison
export M4=$HOME/.local/rootfs/usr/bin/m4

WANT_ALL=0
if [ ${#PHASES[@]} -eq 0 ]; then
    WANT_ALL=1
    PHASES=(musl wayland-native libffi wayland protocols pixman \
            xkeyboard-config xkbcommon \
            libevdev stubs libdrm libinput mesa glib dbus atk gdk-pixbuf \
            cairo pango gtk3 \
            libxfce4util libxfce4windowing xfconf libxfce4ui exo garcon \
            gtk-layer-shell \
            xfce4-panel xfdesktop xfce4-session thunar \
            wlroots labwc weston ffmpeg player)
fi

want() {
    [ $WANT_ALL -eq 1 ] && return 0
    local p
    for p in "${PHASES[@]}"; do [ "$p" = "$1" ] && return 0; done
    return 1
}

stamp() { [ -f "$B/stamp/$1" ]; }
mark() { mkdir -p "$B/stamp" && touch "$B/stamp/$1"; }

# A20OS build-time submodule patches.  Patches live in the OS tree
# ($WL_DIR/patches/<name>-*.patch) and are applied only for the duration of
# the component build, then reversed, so submodule checkouts stay pristine.
a20_patch_apply() {
    local name=$1 p
    for p in "$WL_DIR/patches/$name-"*.patch; do
        [ -e "$p" ] || continue
        git -C "$USER_DIR/external/gui/$name" apply "$p"
    done
}
a20_patch_revert() {
    local name=$1 p
    for p in "$WL_DIR/patches/$name-"*.patch; do
        [ -e "$p" ] || continue
        git -C "$USER_DIR/external/gui/$name" apply -R "$p"
    done
}
a20_patched_build() {
    # a20_patched_build <submodule-name> <build command...>
    local name=$1 rc=0
    shift
    a20_patch_apply "$name"
    "$@" || rc=$?
    a20_patch_revert "$name"
    return $rc
}

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
    env -u ARCH make -C "$MUSL_SH" install
    mark musl
fi

# musl-gcc wrapper (specs-based) as the meson cross compiler
MUSL_GCC=$MUSL_SH/musl-gcc-a20
cat > "$MUSL_GCC" <<EOF
#!/bin/sh
exec $CC -specs "$MUSL_SH/lib/musl-gcc.specs" $ARCH_CFLAGS "\$@"
EOF
chmod +x "$MUSL_GCC"

# musl-g++ wrapper for C++ components (Mesa needs a C++ compiler).
MUSL_CXX=$MUSL_SH/musl-g++-a20
CXX=${CXX:-${CROSS}g++}
cat > "$MUSL_CXX" <<EOF
#!/bin/sh
exec $CXX -specs "$MUSL_SH/lib/musl-gcc.specs" $ARCH_CFLAGS "\$@"
EOF
chmod +x "$MUSL_CXX"

# Prebuilt musl cross toolchain (musl.cc riscv64-linux-musl-cross).  The
# specs-based wrapper above works for C but drops the libstdc++ include
# path for C++; Mesa and the meson-built components use this self-contained
# toolchain (bin/include/lib in its own sysroot) instead.
MUSL_TOOLCHAIN=$BUILD/toolchain/$MUSL_TARGET-linux-musl-cross
if [ -x "$MUSL_TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-gcc" ]; then
    MESON_CC="$MUSL_TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-gcc"
    MESON_CXX="$MUSL_TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-g++"
else
    MESON_CC=$MUSL_GCC
    MESON_CXX=$MUSL_CXX
fi

meson_cross_ini() {
    cat > "$B/meson-cross.ini" <<EOF
[host_machine]
system = 'linux'
cpu_family = '$CPU_FAMILY'
cpu = '$ARCH'
endian = 'little'

[binaries]
c = '$MESON_CC'
cpp = '$MESON_CXX'
ar = '$AR'
strip = '${CROSS}strip'
pkgconfig = 'pkg-config'
glib-compile-resources = '/usr/bin/glib-compile-resources'
glib-mkenums = '/usr/bin/glib-mkenums'
glib-genmarshal = '/usr/bin/glib-genmarshal'
glib-compile-schemas = '/usr/bin/glib-compile-schemas'
gdbus-codegen = '/usr/bin/gdbus-codegen'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$SYSROOT/lib/pkgconfig', '$SYSROOT/share/pkgconfig']

[built-in options]
c_args = ['-O2', '-D_GNU_SOURCE', '-fPIC', '-I$SYSROOT/include', '-I$SYSROOT/include/libdrm']
c_link_args = ['-fPIC', '-L$SYSROOT/lib', '-Wl,-rpath-link,$SYSROOT/lib']
cpp_args = ['-O2', '-D_GNU_SOURCE', '-fPIC', '-I$SYSROOT/include', '-I$SYSROOT/include/libdrm']
cpp_link_args = ['-fPIC', '-L$SYSROOT/lib', '-Wl,-rpath-link,$SYSROOT/lib']
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
    meson setup "$B/build-wayland-native" "$USER_DIR/external/gui/wayland" \
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

# autotools_pkg <name> <srcdir> <configure args...>
# Runs autogen (when the source is a git checkout) then cross-configures,
# builds and installs an autotools component into the sysroot.  Builds
# in-tree from a copy of the source so generated headers (xfconf-alias.h
# etc.) are found by #include without VPATH headaches.
autotools_pkg() {
    local name=$1 src=$2
    shift 2
    # Minimal X11 header stubs: several XFCE components still include X11
    # headers (session management, atoms) even on the Wayland build.
    mkdir -p "$SYSROOT/include/X11/SM" "$SYSROOT/include/X11/ICE"
    [ -f "$SYSROOT/include/X11/Xlib.h" ] || cat > "$SYSROOT/include/X11/Xlib.h" <<'XEOF'
#ifndef _XLIB_H_
#define _XLIB_H_
typedef struct _XDisplay Display;
#endif
XEOF
    [ -f "$SYSROOT/include/X11/Xutil.h" ] || cat > "$SYSROOT/include/X11/Xutil.h" <<'XEOF'
#ifndef _XUTIL_H_
#define _XUTIL_H_
#endif
XEOF
    [ -f "$SYSROOT/include/X11/Xatom.h" ] || cat > "$SYSROOT/include/X11/Xatom.h" <<'XEOF'
#ifndef _XATOM_H_
#define _XATOM_H_
#define XA_CARDINAL 6L
#define XA_ATOM 4L
#define XA_WINDOW 33L
#endif
XEOF
    [ -f "$SYSROOT/include/X11/ICE/ICElib.h" ] || cat > "$SYSROOT/include/X11/ICE/ICElib.h" <<'XEOF'
#ifndef _ICE_LIB_H_
#define _ICE_LIB_H_
typedef struct _IceConn *IceConn;
#endif
XEOF
    [ -f "$SYSROOT/include/X11/SM/SMlib.h" ] || cat > "$SYSROOT/include/X11/SM/SMlib.h" <<'XEOF'
#ifndef _SM_LIB_H_
#define _SM_LIB_H_
typedef struct _SmcConn *SmcConn;
typedef struct _SmsConn *SmsConn;
typedef enum {
    SmRestartIfRunning = 0,
    SmRestartAnyway = 1,
    SmRestartImmediately = 2,
    SmRestartNever = 3
} SmRestartStyle;
#define SmRestartStyleHint "SmRestartStyleHint"
#define SmProgram          "Program"
#define SmProcessID        "ProcessID"
#endif
XEOF
    if [ -x "$src/autogen.sh" ] && [ ! -x "$src/configure" ]; then
        (cd "$src" && NOCONFIGURE=1 ./autogen.sh >/dev/null 2>&1 || \
         cd "$src" && ./autogen.sh --noconfigure >/dev/null 2>&1)
    fi
    [ -x "$src/configure" ] || { echo "autotools_pkg: no configure for $name" >&2; return 1; }
    local OB=$B/build-$name
    rm -rf "$OB" && mkdir -p "$OB"
    cp -a "$src"/. "$OB"/
    (cd "$OB" && ./configure \
        --host="$MUSL_TARGET-linux-musl" \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --includedir="$SYSROOT/include" \
        --disable-static --enable-shared \
        --disable-doc --disable-docs --disable-gtk-doc \
        --enable-maintainer-mode \
        CC="$MESON_CC" CXX="$MESON_CXX" \
        CPPFLAGS="-I$SYSROOT/include -I$SYSROOT/include/gtk-3.0 -I$SYSROOT/include/glib-2.0 -I$SYSROOT/lib/glib-2.0/include -I$SYSROOT/include/atk-1.0 -I$SYSROOT/include/pango-1.0 -I$SYSROOT/include/cairo -I$SYSROOT/include/libxfce4util" \
        LDFLAGS="-L$SYSROOT/lib -Wl,-rpath-link,$SYSROOT/lib" \
        PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig" \
        GLIB_COMPILE_RESOURCES=/usr/bin/glib-compile-resources \
        GLIB_GENMARSHAL=/usr/bin/glib-genmarshal \
        GLIB_MKENUMS=/usr/bin/glib-mkenums \
        GDBUS_CODEGEN=/usr/bin/gdbus-codegen \
        "$@" 2>&1 | tail -5)
    env -u ARCH -u MAKEFLAGS make -C "$OB" -j"$(nproc)" 2>&1 | tail -3
    env -u ARCH -u MAKEFLAGS make -C "$OB" install 2>&1 | tail -3
}

# ---------------------------------------------------------------- libffi
if want libffi && ! stamp libffi; then
    echo "=== libffi ==="
    mkdir -p "$SYSROOT/include/linux"
    cp /usr/include/linux/limits.h "$SYSROOT/include/linux/limits.h"
    SRC=$BUILD/libffi-3.4.6
    OB=$B/build-libffi
    rm -rf "$OB" && mkdir -p "$OB"
    (cd "$OB" && "$SRC/configure" \
        --host="$LIBFFI_HOST" \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --includedir="$SYSROOT/include" \
        --disable-static --disable-docs --disable-exec-static-tramp \
        CC="$MUSL_GCC" CFLAGS="-O2 -fPIC" \
        CPPFLAGS="-I$SYSROOT/include")
    env -u ARCH -u MAKEFLAGS make -C "$OB" -j"$(nproc)"
    env -u ARCH -u MAKEFLAGS make -C "$OB" install
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
    meson_pkg wayland "$USER_DIR/external/gui/wayland" \
        -Ddocumentation=false -Dtests=false -Ddtd_validation=false \
        -Dscanner=false
    mark wayland
fi

# ------------------------------------------------------- wayland-protocols
if want protocols && ! stamp protocols; then
    echo "=== wayland-protocols ==="
    meson_pkg wayland-protocols "$USER_DIR/external/gui/wayland-protocols" \
        -Dtests=false
    mark protocols
fi

# ---------------------------------------------------------------- pixman
if want pixman && ! stamp pixman; then
    echo "=== pixman ==="
    meson_pkg pixman "$USER_DIR/external/gui/pixman" \
        -Dtests=disabled -Ddemos=disabled -Dgtk=disabled \
        -Dlibpng=disabled -Dgnuplot=false -Dopenmp=disabled
    mark pixman
fi

# ------------------------------------------------------ xkeyboard-config
if want xkeyboard-config && ! stamp xkeyboard-config; then
    echo "=== xkeyboard-config (native data) ==="
    rm -rf "$USER_DIR/build/xkeyboard-config"
    meson setup "$USER_DIR/build/xkeyboard-config" \
        "$USER_DIR/external/gui/xkeyboard-config"
    ninja -C "$USER_DIR/build/xkeyboard-config"
    mark xkeyboard-config
fi

# ------------------------------------------------------------ libxkbcommon
if want xkbcommon && ! stamp xkbcommon; then
    echo "=== libxkbcommon ==="
    meson_pkg libxkbcommon "$USER_DIR/external/gui/libxkbcommon" \
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
    mkdir -p "$SYSROOT/include"
    cp -R /usr/include/linux /usr/include/asm-generic "$SYSROOT/include/"
    rm -rf "$SYSROOT/include/asm"
    cp -RL "$UAPI_ASM" "$SYSROOT/include/asm"
    for h in input.h input-event-codes.h uinput.h; do
        [ -f "/usr/$MUSL_TARGET-linux-gnu/include/linux/$h" ] && \
            cp "/usr/$MUSL_TARGET-linux-gnu/include/linux/$h" \
               "$SYSROOT/include/linux/"
    done
    SRC=$USER_DIR/external/gui/libevdev
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
    cp "$WL_DIR/stub/mtdev.h" "$SYSROOT/include/mtdev-plumbing.h"
    "$MUSL_GCC" -O2 -fPIC -I"$WL_DIR/stub" -I"$SYSROOT/include" \
        -c "$WL_DIR/stub/mtdev.c" -o "$OB/mtdev.o"
    "$MUSL_GCC" -shared -Wl,-soname,libmtdev.so.1 \
        -o "$OB/libmtdev.so.1.1.6" "$OB/mtdev.o"
    cp "$OB/libmtdev.so.1.1.6" "$SYSROOT/lib/"
    ln -sf libmtdev.so.1.1.6 "$SYSROOT/lib/libmtdev.so.1"
    ln -sf libmtdev.so.1.1.6 "$SYSROOT/lib/libmtdev.so"
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
    echo "=== libdrm ==="
    meson_pkg libdrm "$USER_DIR/external/gui/libdrm" \
        -Dtests=false -Dudev=false -Dinstall-test-programs=false \
        -Dman-pages=disabled -Dvalgrind=disabled \
        -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
        -Dnouveau=disabled -Dvmwgfx=disabled -Dexynos=disabled \
        -Dtegra=disabled -Dfreedreno=disabled -Detnaviv=disabled \
        -Dvc4=disabled -Domap=disabled
    mark libdrm
fi

# --------------------------------------------------------------- libinput
if want libinput && ! stamp libinput; then
    echo "=== libinput ==="
    meson_pkg libinput "$USER_DIR/external/gui/libinput" \
        -Dlibwacom=false -Ddebug-gui=false -Dtests=false \
        -Ddocumentation=false -Dinstall-tests=false \
        -Dzshcompletiondir=no
    mark libinput
fi

# ------------------------------------------------------------------ mesa
if want mesa && ! stamp mesa; then
    echo "=== mesa (EGL/GLES + gbm + virgl/softpipe) ==="
    export BISON_PKGDATADIR="$HOME/.local/rootfs/usr/share/bison"
    export M4="$HOME/.local/rootfs/usr/bin/m4"
    meson_pkg mesa "$USER_DIR/external/gui/mesa" \
        -Dgallium-drivers=virgl,softpipe \
        -Dvulkan-drivers= \
        -Dglx=disabled -Dgbm=enabled -Degl=enabled \
        -Dgles1=disabled -Dgles2=enabled \
        -Dplatforms= -Dllvm=disabled -Dopengl=true \
        -Dgallium-extra-hud=false -Dtools= -Dbuild-tests=false \
        -Dshader-cache=disabled
    # Mesa runtime deps that live in the prebuilt toolchain's lib dir:
    # libstdc++ (musl-native) and libgcc_s, plus stripping the 96 MB
    # libgallium megadriver down to a deployable size.
    TC_LIB="$MUSL_TOOLCHAIN/$MUSL_TARGET-linux-musl/lib"
    if [ -f "$TC_LIB/libstdc++.so.6.0.29" ]; then
        cp "$TC_LIB/libstdc++.so.6.0.29" "$SYSROOT/lib/"
        ln -sf libstdc++.so.6.0.29 "$SYSROOT/lib/libstdc++.so.6"
    fi
    if [ -f "$TC_LIB/libgcc_s.so.1" ]; then
        cp "$TC_LIB/libgcc_s.so.1" "$SYSROOT/lib/"
    fi
    if [ -x "$MUSL_TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-strip" ]; then
        "$MUSL_TOOLCHAIN/bin/$MUSL_TARGET-linux-musl-strip" --strip-unneeded \
            "$SYSROOT/lib/libgallium-"*.so \
            "$SYSROOT/lib/libEGL.so."* "$SYSROOT/lib/libGLESv2.so."* \
            "$SYSROOT/lib/libGLESv1_CM.so."* "$SYSROOT/lib/libgbm.so."* \
            "$SYSROOT/lib/libstdc++.so.6.0.29" 2>/dev/null || true
    fi
    mark mesa
fi

# ------------------------------------------------------------------ glib
if want glib && ! stamp glib; then
    echo "=== glib ==="
    # glib 2.84 needs a gvdb with gvdb_table_n_children and the tests /
    # build-tools options; the pinned gvdb.wrap revision predates that.
    GVDB_DIR="$USER_DIR/external/gui/glib/subprojects/gvdb"
    if [ -d "$GVDB_DIR/.git" ]; then
        (cd "$GVDB_DIR" && git stash -q 2>/dev/null; \
         git checkout -q c6f2359 2>/dev/null || true; \
         rm -f meson.options; \
         grep -q "build-tools" meson_options.txt 2>/dev/null || \
             echo "option('build-tools', type : 'boolean', value : false)" >> meson_options.txt)
    fi
    meson_pkg glib "$USER_DIR/external/gui/glib" \
        -Dtests=false -Dinstalled_tests=false -Dglib_assert=false \
        -Dglib_checks=false -Dlibmount=disabled -Dlibelf=disabled \
        -Dman=false -Ddocumentation=false -Ddtrace=false
    mark glib
fi

# ------------------------------------------------------------------ dbus
if want dbus && ! stamp dbus; then
    echo "=== dbus (cmake) ==="
    OB=$B/build-dbus
    rm -rf "$OB" && mkdir -p "$OB"
    (cd "$OB" && cmake "$USER_DIR/external/gui/dbus" \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$CPU_FAMILY" \
        -DCMAKE_C_COMPILER="$MESON_CC" -DCMAKE_CXX_COMPILER="$MESON_CXX" \
        -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_PREFIX_PATH="$SYSROOT" \
        -DEXPAT_INCLUDE_DIR="$SYSROOT/include" \
        -DEXPAT_LIBRARY="$SYSROOT/lib/libexpat.so" \
        -DDBUS_BUILD_TESTS=OFF -DDBUS_BUILD_X11=OFF \
        -DENABLE_SYSTEMD=OFF -DENABLE_LAUNCHD=OFF \
        -DDBUS_SESSION_SOCKET_DIR=/tmp \
        -DCMAKE_C_FLAGS="-I$SYSROOT/include" > /dev/null)
    # dbus 1.14 cmake leaks a -lsystemd into every link even with
    # ENABLE_SYSTEMD=OFF; strip it so the toolchain does not need systemd.
    find "$OB" -name "link.txt" -exec sed -i 's/ -lsystemd//g' {} \;
    cmake --build "$OB" --target dbus-daemon dbus-1 dbus-send \
        dbus-run-session dbus-uuidgen -j"$(nproc)" 2>&1 | tail -3
    cmake --install "$OB" 2>/dev/null || true
    cp -r "$USER_DIR/external/gui/dbus"/dbus/*.h "$SYSROOT/include/dbus/" 2>/dev/null || true
    mkdir -p "$SYSROOT/lib/pkgconfig"
    cat > "$SYSROOT/lib/pkgconfig/dbus-1.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include
Name: dbus
Description: D-Bus message bus
Version: 1.14.10
Libs: -L\${libdir} -ldbus-1
Cflags: -I\${includedir}/dbus-1.0 -I\${libdir}/dbus-1.0/include
EOF
    mark dbus
fi

# ------------------------------------------------------------------ atk
if want atk && ! stamp atk; then
    echo "=== atk ==="
    meson_pkg atk "$USER_DIR/external/gui/atk" \
        -Dintrospection=false -Ddocs=false
    mark atk
fi

# ------------------------------------------------------------------ gdk-pixbuf
if want gdk-pixbuf && ! stamp gdk-pixbuf; then
    echo "=== gdk-pixbuf ==="
    meson_pkg gdk-pixbuf "$USER_DIR/external/gui/gdk-pixbuf" \
        -Dtests=false -Dinstalled_tests=false -Dgtk_doc=false \
        -Dintrospection=disabled -Dman=false -Dbuiltin_loaders=png \
        -Drelocatable=true -Dgio_sniffing=false -Djpeg=false \
        -Dtiff=false -Dgif=false
    mark gdk-pixbuf
fi

# ------------------------------------------------------------------ cairo
if want cairo && ! stamp cairo; then
    echo "=== cairo ==="
    meson_pkg cairo "$USER_DIR/external/gui/cairo" \
        -Dfontconfig=enabled -Dfreetype=enabled -Dpng=enabled \
        -Dzlib=enabled -Dxlib=disabled -Dxlib-xcb=disabled \
        -Dxcb=disabled -Dquartz=disabled -Ddwrite=disabled \
        -Dtee=disabled
    mark cairo
fi

# ------------------------------------------------------------------ pango
if want pango && ! stamp pango; then
    echo "=== pango ==="
    meson_pkg pango "$USER_DIR/external/gui/pango" \
        -Dbuild-testsuite=false -Dbuild-examples=false \
        -Ddocumentation=false -Dgtk_doc=false -Dintrospection=disabled \
        -Dcairo=enabled -Dfreetype=enabled -Dfontconfig=enabled
    mark pango
fi

# ------------------------------------------------------------------ gtk3
if want gtk3 && ! stamp gtk3; then
    echo "=== gtk3 ==="
    meson_pkg gtk3 "$USER_DIR/external/gui/gtk3" \
        -Dgtk_doc=false -Dman=false -Dtests=false -Dinstalled_tests=false \
        -Ddemos=false -Dexamples=false \
        -Dintrospection=false \
        -Dx11_backend=false -Dwayland_backend=true \
        -Dbroadway_backend=false -Dquartz_backend=false \
        -Dwin32_backend=false -Dcloudproviders=false \
        -Dprint_backends=file -Dcolord=no \
        -Dlibepoxy:glx=no
    mark gtk3
fi

# ------------------------------------------------------------------ XFCE
# libxfce4util and libxfce4windowing ship meson builds; the rest are
# autotools (autogen'd in autotools_pkg).
if want libxfce4util && ! stamp libxfce4util; then
    echo "=== libxfce4util ==="
    meson_pkg libxfce4util "$USER_DIR/external/gui/libxfce4util" \
        -Dgtk-doc=false -Dintrospection=false -Dvala=disabled
    mark libxfce4util
fi

if want libxfce4windowing && { ! stamp libxfce4windowing || \
    [ "$WL_DIR/patches/libxfce4windowing-a20.patch" -nt "$B/stamp/libxfce4windowing" ]; }; then
    echo "=== libxfce4windowing ==="
    # A20OS patch: do not bind ext_workspace_manager_v1 (protocol lifetime
    # disagreement with the bundled wlroots); xfdesktop only needs the dummy
    # workspace manager.  Applied at build time only.
    a20_patched_build libxfce4windowing \
        meson_pkg libxfce4windowing "$USER_DIR/external/gui/libxfce4windowing" \
        -Dgtk-doc=false -Dintrospection=false -Dtests=false \
        -Dwayland=enabled -Dx11=disabled
    mark libxfce4windowing
fi

if want xfconf && ! stamp xfconf; then
    echo "=== xfconf ==="
    autotools_pkg xfconf "$USER_DIR/external/gui/xfconf"
    mark xfconf
fi

if want libxfce4ui && ! stamp libxfce4ui; then
    echo "=== libxfce4ui ==="
    autotools_pkg libxfce4ui "$USER_DIR/external/gui/libxfce4ui"
    mark libxfce4ui
fi

if want exo && ! stamp exo; then
    echo "=== exo ==="
    autotools_pkg exo "$USER_DIR/external/gui/exo"
    mark exo
fi

if want garcon && ! stamp garcon; then
    echo "=== garcon ==="
    autotools_pkg garcon "$USER_DIR/external/gui/garcon"
    mark garcon
fi

if want gtk-layer-shell && ! stamp gtk-layer-shell; then
    echo "=== gtk-layer-shell ==="
    meson_pkg gtk-layer-shell "$USER_DIR/external/gui/gtk-layer-shell" \
        -Dexamples=false -Dtests=false -Dintrospection=false \
        -Ddocs=false
    mark gtk-layer-shell
fi

# ------------------------------------------------------------------ wlroots
if want wlroots && ! stamp wlroots; then
    echo "=== wlroots ==="
    "$WL_DIR/build-compositor.sh" "$ARCH" wlroots
    mark wlroots
fi

# ------------------------------------------------------------------ labwc
if want labwc && ! stamp labwc; then
    echo "=== labwc ==="
    "$WL_DIR/build-compositor.sh" "$ARCH" labwc
    mark labwc
fi

if want xfce4-panel && ! stamp xfce4-panel; then
    echo "=== xfce4-panel ==="
    # The panel's external-plugin wrapper path (HELPERDIR, default $libdir)
    # is compiled into the binary.  $libdir is the build host's absolute
    # sysroot path, which does not exist inside the guest where the FAT root
    # is mounted at /bin.  The guest-visible wrapper is
    # /bin/lib/xfce4/panel/wrapper-2.0 (image path /lib/xfce4/panel/wrapper-2.0),
    # so HELPERDIR must be /bin/lib.  Without this, every spawn of an
    # external panel plugin fails with "Failed to spawn the
    # xfce4-panel-wrapper".
    autotools_pkg xfce4-panel "$USER_DIR/external/gui/xfce4-panel" \
        --with-helper-path-prefix=/bin/lib
    mark xfce4-panel
fi

if want xfdesktop && { ! stamp xfdesktop || \
    [ "$B/stamp/libxfce4windowing" -nt "$B/stamp/xfdesktop" ]; }; then
    echo "=== xfdesktop ==="
    # Wayland build: skip the XSMP session-management option group (an X11
    # feature; libxfce4ui's xfce-sm-client symbol is not exported here).
    # Applied at build time only, via patches/xfdesktop-a20.patch.
    a20_patched_build xfdesktop \
        autotools_pkg xfdesktop "$USER_DIR/external/gui/xfdesktop" \
        CFLAGS="-O2 -DA20_NO_X11_SESSION"
    mark xfdesktop
fi

if want xfce4-session && ! stamp xfce4-session; then
    echo "=== xfce4-session ==="
    autotools_pkg xfce4-session "$USER_DIR/external/gui/xfce4-session" \
        --with-wayland-session-prefix="$SYSROOT"
    mark xfce4-session
fi

if want thunar && ! stamp thunar; then
    echo "=== thunar ==="
    # Wayland build: thunar 4.20 still calls XDT_CHECK_LIBX11_REQUIRE;
    # relax it to the optional check so the X11-less build configures.
    # Applied at build time only, via patches/thunar-a20.patch.
    a20_patched_build thunar \
        autotools_pkg thunar "$USER_DIR/external/gui/thunar" \
        --disable-wallpaper-plugin
    mark thunar
fi

# ---------------------------------------------------------------- weston
if want weston && ! stamp weston; then
    echo "=== weston ==="
    for p in "$WL_DIR/patches/weston-"*.patch; do
        (cd "$USER_DIR/external/gui/weston" && \
            git apply --unidiff-zero --check "$p" 2>/dev/null && \
            git apply --unidiff-zero "$p") || true
    done
    meson_pkg weston "$USER_DIR/external/gui/weston" \
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

# ---------------------------------------------------------------- ffmpeg
if want ffmpeg && ! stamp ffmpeg; then
    echo "=== ffmpeg ==="
    SRC=$USER_DIR/external/libs/ffmpeg
    OB=$B/build-ffmpeg
    rm -rf "$OB" && mkdir -p "$OB"
    # FFmpeg 7.1 H.264 SEI shares its AOM film-grain object with HEVC.
    (cd "$OB" && "$SRC/configure" \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --arch="$CPU_FAMILY" --target-os=linux --enable-cross-compile \
        --cross-prefix="$CROSS" --cc="$MUSL_GCC" --ar="$AR" \
        --strip="${CROSS}strip" \
        --enable-shared --disable-static --disable-programs \
        --disable-doc --disable-debug --disable-autodetect \
        --disable-network --disable-avdevice --disable-avfilter \
        --disable-postproc --disable-encoders --disable-muxers \
        --disable-bsfs --disable-protocols --enable-protocol=file \
        --disable-demuxers --enable-demuxer=mov \
        --disable-decoders --enable-decoder=h264,aac,hevc \
        --disable-parsers --enable-parser=h264,aac \
        --disable-hwaccels --disable-filters --disable-devices \
        --disable-iconv --disable-zlib --disable-bzlib --disable-lzma \
        --disable-asm --disable-inline-asm --disable-neon --enable-pthreads \
        --extra-cflags="-O2 -fPIC -I$SYSROOT/include" \
        --extra-ldflags="-L$SYSROOT/lib")
    env -u ARCH -u MAKEFLAGS make -C "$OB" -j"$(nproc)"
    env -u ARCH -u MAKEFLAGS make -C "$OB" install
    mark ffmpeg
fi

# ----------------------------------------------------------- media player
if want player && ! stamp player; then
    echo "=== a20-player ==="
    OB=$B/build-player
    rm -rf "$OB" && mkdir -p "$OB"
    SCANNER=$HOST_TOOLS/wayland/bin/wayland-scanner
    XDG_XML=$SYSROOT/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    "$SCANNER" client-header "$XDG_XML" "$OB/xdg-shell-client-protocol.h"
    "$SCANNER" private-code "$XDG_XML" "$OB/xdg-shell-protocol.c"
    DESKTOP_XML=$USER_DIR/external/gui/weston/protocol/weston-desktop-shell.xml
    "$SCANNER" client-header "$DESKTOP_XML" \
        "$OB/weston-desktop-shell-client-protocol.h"
    "$SCANNER" private-code "$DESKTOP_XML" \
        "$OB/weston-desktop-shell-protocol.c"
    mkdir -p "$SYSROOT/include/uapi/a20" "$SYSROOT/bin"
    cp "$USER_DIR/../kernel/include/uapi/a20/audio.h" \
       "$SYSROOT/include/uapi/a20/audio.h"
    "$MUSL_GCC" -O2 -D_GNU_SOURCE -I"$OB" -I"$SYSROOT/include" \
        -c "$WL_DIR/player.c" -o "$OB/player.o"
    "$MUSL_GCC" -O2 -D_GNU_SOURCE -I"$OB" -I"$SYSROOT/include" \
        -c "$OB/xdg-shell-protocol.c" -o "$OB/xdg-shell-protocol.o"
    "$MUSL_GCC" -O2 -D_GNU_SOURCE -I"$OB" -I"$SYSROOT/include" \
        -c "$WL_DIR/desktop-shell.c" -o "$OB/desktop-shell.o"
    "$MUSL_GCC" -O2 -D_GNU_SOURCE -I"$OB" -I"$SYSROOT/include" \
        -c "$OB/weston-desktop-shell-protocol.c" \
        -o "$OB/weston-desktop-shell-protocol.o"
    "$MUSL_GCC" -o "$SYSROOT/bin/a20-player" \
        "$OB/player.o" "$OB/xdg-shell-protocol.o" \
        -Wl,-rpath-link,"$SYSROOT/lib" \
        -L"$SYSROOT/lib" -lavformat -lavcodec -lswresample -lswscale \
         -lavutil -lwayland-client -lffi -lpthread -lm
    "$MUSL_GCC" -o "$SYSROOT/bin/a20-desktop-shell" \
        "$OB/desktop-shell.o" "$OB/weston-desktop-shell-protocol.o" \
        -Wl,-rpath-link,"$SYSROOT/lib" -L"$SYSROOT/lib" \
        -lwayland-client -lffi -lpthread -lm
    "$MUSL_GCC" -O2 -D_GNU_SOURCE -I"$SYSROOT/include" \
        "$WL_DIR/input-method.c" -o "$SYSROOT/bin/a20-input-method" \
        -Wl,-rpath-link,"$SYSROOT/lib" -L"$SYSROOT/lib" \
        -lwayland-client -lffi -lpthread -lm
    "$MUSL_GCC" -O2 -static "$USER_DIR/cmds/core/wayland-session.c" \
        -o "$SYSROOT/bin/wayland-session"
    # EGL/GLES smoke client: links against the Mesa libs in the sysroot.
    "$MESON_CC" -O2 -I"$SYSROOT/include" \
        "$USER_DIR/cmds/core/egl_test.c" -o "$SYSROOT/bin/egl_test" \
        -Wl,-rpath-link,"$SYSROOT/lib" -L"$SYSROOT/lib" \
        -lEGL -lGLESv2 -lpthread -lm
    mark player
fi

echo "=== done ==="
