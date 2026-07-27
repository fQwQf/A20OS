#!/usr/bin/env bash
# Install the wayland/weston stack onto an A20OS FAT32 disk image.
# Usage: user/wayland/install-image.sh <fat32.img> [ARCH]
set -euo pipefail

IMG=$1
ARCH=${2:-riscv64}
USER_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=$USER_DIR/build/wayland
SYSROOT=$BUILD/$ARCH/sysroot
MUSL_SH=$BUILD/musl-$ARCH

[ -f "$IMG" ] || { echo "image not found: $IMG" >&2; exit 1; }

copy_file() { # copy_file <src> <dst-path-in-image>
    local src=$1 dst=$2 dir part cur
    dir=$(dirname "$dst")
    cur=""
    IFS='/' read -ra part <<< "$dir"
    for p in "${part[@]}"; do
        [ -n "$p" ] || continue
        cur="$cur/$p"
        mmd -i "$IMG" "$cur" >/dev/null 2>&1 || true
    done
    mcopy -o -i "$IMG" "$src" "::${dst}"
}

copy_soname() { # copy_soname <libfile.so.X.Y.Z> <soname>
    local real=$1 soname=$2
    copy_file "$SYSROOT/lib/$real" "/lib/$soname"
}

echo "[image] musl runtime"
copy_file "$MUSL_SH/install/lib/libc.so" /lib/libc.so
case $ARCH in
    riscv64) copy_file "$MUSL_SH/install/lib/libc.so" /lib/ld-musl-riscv64.so.1 ;;
    aarch64) copy_file "$MUSL_SH/install/lib/libc.so" /lib/ld-musl-aarch64.so.1 ;;
    x86_64)  copy_file "$MUSL_SH/install/lib/libc.so" /lib/ld-musl-x86_64.so.1 ;;
esac

echo "[image] wayland stack libs"
copy_soname "$(cd "$SYSROOT/lib" && ls libwayland-server.so.0.*.* | head -1)" libwayland-server.so.0
copy_soname "$(cd "$SYSROOT/lib" && ls libwayland-client.so.0.*.* | head -1)" libwayland-client.so.0
copy_soname "$(cd "$SYSROOT/lib" && ls libwayland-cursor.so.0.*.* | head -1)" libwayland-cursor.so.0
copy_soname "$(cd "$SYSROOT/lib" && ls libffi.so.8.*.* | head -1)" libffi.so.8
copy_soname "$(cd "$SYSROOT/lib" && ls libpixman-1.so.0.*.* | head -1)" libpixman-1.so.0
copy_soname "$(cd "$SYSROOT/lib" && ls libxkbcommon.so.0.*.* | head -1)" libxkbcommon.so.0
copy_soname "$(cd "$SYSROOT/lib" && ls libinput.so.10.*.* | head -1)" libinput.so.10
copy_soname "$(cd "$SYSROOT/lib" && ls libevdev.so.2.*.* | head -1)" libevdev.so.2
copy_soname "$(cd "$SYSROOT/lib" && ls libudev.so.1.*.* | head -1)" libudev.so.1
copy_soname "$(cd "$SYSROOT/lib" && ls libmtdev.so.1.*.* | head -1)" libmtdev.so.1

echo "[image] weston libs + modules"
copy_soname "$(cd "$SYSROOT/lib" && ls libweston-9.so.0.*.* | head -1)" libweston-9.so.0
copy_soname "$(cd "$SYSROOT/lib" && ls libweston-desktop-9.so.0.*.* | head -1)" libweston-desktop-9.so.0
copy_file "$SYSROOT/lib/weston/$(cd "$SYSROOT/lib/weston" && ls libexec_weston.so.0.*.* | head -1)" /lib/libexec_weston.so.0
copy_file "$SYSROOT/lib/libweston-9/fbdev-backend.so" /lib/libweston-9/fbdev-backend.so
copy_file "$SYSROOT/lib/weston/desktop-shell.so" /lib/weston/desktop-shell.so

echo "[image] weston binaries"
copy_file "$SYSROOT/bin/weston" /weston
copy_file "$SYSROOT/bin/weston-simple-shm" /weston-simple-shm

echo "[image] libinput quirks"
if [ -d "$SYSROOT/share/libinput" ]; then
    (cd "$SYSROOT/share/libinput" && find . -type f | sed "s|^\./||") | while read -r f; do
        copy_file "$SYSROOT/share/libinput/$f" "/share/libinput/$f"
    done
fi

echo "[image] xkeyboard-config data"
XKBC=$USER_DIR/external/xkeyboard-config
for d in keycodes types compat symbols geometry rules; do
    [ -d "$XKBC/$d" ] || continue
    (cd "$XKBC/$d" && find . -type f | sed "s|^\./||") | while read -r f; do
        copy_file "$XKBC/$d/$f" "/usr/share/X11/xkb/$d/$f"
    done
done

echo "[image] weston.ini"
printf '[core]\nrequire-input=false\n[shell]\nbackground-color=0xff002244\n' | \
    mcopy -o -i "$IMG" - ::/etc/xdg/weston.ini 2>/dev/null || {
    mmd -i "$IMG" ::/etc/xdg >/dev/null 2>&1
    printf '[core]\nrequire-input=false\n[shell]\nbackground-color=0xff002244\n' | \
        mcopy -o -i "$IMG" - ::/etc/xdg/weston.ini
}

echo "[image] run-weston.sh"
# Launcher recipe (FAT32 mounts at /bin at runtime):
#  - modules live in /bin/lib/libweston-9 and /bin/lib/weston
#  - xkb data in /bin/usr/share/X11/xkb (XKB_CONFIG_ROOT)
#  - seat1 avoids the VT setup in launcher-direct (no VTs on A20OS)
cat > /tmp/opencode/run-weston-$$.sh <<'EOS'
#!/bin/sh
export XDG_RUNTIME_DIR=/tmp
export XKB_CONFIG_ROOT=/bin/usr/share/X11/xkb
export XDG_CONFIG_DIRS=/bin/etc/xdg
export WESTON_MODULE_MAP="fbdev-backend.so=/bin/lib/libweston-9/fbdev-backend.so;kiosk-shell.so=/bin/lib/weston/kiosk-shell.so;desktop-shell.so=/bin/lib/weston/desktop-shell.so"
exec weston --backend=fbdev-backend.so --device=/dev/fb0 --seat=seat1 --shell=kiosk-shell.so "$@"
EOS
copy_file /tmp/opencode/run-weston-$$.sh /run-weston.sh
rm -f /tmp/opencode/run-weston-$$.sh

echo "[image] done"
