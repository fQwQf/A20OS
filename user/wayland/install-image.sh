#!/usr/bin/env bash
# Install the wayland/weston stack onto an A20OS FAT32 disk image.
# Usage: user/wayland/install-image.sh <fat32.img> [ARCH] [media.mp4]
set -euo pipefail

# A second GUI build must fail quickly instead of leaving the terminal waiting
# inside mtools while another process owns the image.
export MTOOLS_LOCK_TIMEOUT=${MTOOLS_LOCK_TIMEOUT:-5}

IMG=$1
ARCH=${2:-riscv64}
USER_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MEDIA=${3:-$USER_DIR/external/lvgl/tests/src/test_assets/test_video_birds.mp4}
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
        mmd -D s -i "$IMG" "$cur" >/dev/null 2>&1 || true
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
    loongarch64) copy_file "$MUSL_SH/install/lib/libc.so" /lib/ld-musl-loongarch64.so.1 ;;
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

if [ -f "$SYSROOT/bin/a20-player" ]; then
    echo "[image] FFmpeg + media player"
    copy_soname "$(cd "$SYSROOT/lib" && ls libavformat.so.61.*.* | head -1)" libavformat.so.61
    copy_soname "$(cd "$SYSROOT/lib" && ls libavcodec.so.61.*.* | head -1)" libavcodec.so.61
    copy_soname "$(cd "$SYSROOT/lib" && ls libswresample.so.5.*.* | head -1)" libswresample.so.5
    copy_soname "$(cd "$SYSROOT/lib" && ls libswscale.so.8.*.* | head -1)" libswscale.so.8
    copy_soname "$(cd "$SYSROOT/lib" && ls libavutil.so.59.*.* | head -1)" libavutil.so.59
    copy_file "$SYSROOT/bin/a20-player" /a20-player
    copy_file "$SYSROOT/bin/wayland-session" /wayland-session
    if [ -f "$MEDIA" ]; then
        copy_file "$MEDIA" /media/demo.mp4
    else
        echo "[image] WARNING: media file not found: $MEDIA" >&2
    fi
fi

if [ -f "$SYSROOT/lib/libcairo.so.2" ]; then
    echo "[image] cairo + font stack"
    copy_file "$SYSROOT/lib/libcairo.so.2" /lib/libcairo.so.2
    copy_file "$SYSROOT/lib/libfontconfig.so.1" /lib/libfontconfig.so.1
    copy_file "$SYSROOT/lib/libfreetype.so.6" /lib/libfreetype.so.6
    copy_file "$SYSROOT/lib/libpng16.so.16" /lib/libpng16.so.16
    copy_file "$SYSROOT/lib/libexpat.so.1" /lib/libexpat.so.1
    copy_file "$SYSROOT/lib/libz.so.1" /lib/libz.so.1

    copy_file /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
        /usr/share/fonts/DejaVuSans.ttf
    copy_file /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf \
        /usr/share/fonts/DejaVuSans-Bold.ttf
    copy_file /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
        /usr/share/fonts/DejaVuSansMono.ttf
    copy_file /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf \
        /usr/share/fonts/DejaVuSansMono-Bold.ttf

    FONTCONF_TMP=/tmp/opencode/fonts.conf-$$
    sed -e 's|<dir>/usr/share/fonts</dir>|<dir>/bin/usr/share/fonts</dir>|' \
        -e 's|<dir>/usr/local/share/fonts</dir>|<dir>/bin/usr/share/fonts</dir>|' \
        -e 's|<cachedir>.*var/cache/fontconfig</cachedir>|<cachedir>/tmp/fontconfig</cachedir>|' \
        "$SYSROOT/etc/fonts/fonts.conf" > "$FONTCONF_TMP"
    copy_file "$FONTCONF_TMP" /etc/fonts/fonts.conf
    rm -f "$FONTCONF_TMP"
fi

echo "[image] weston libs + modules"
copy_soname "$(cd "$SYSROOT/lib" && ls libweston-9.so.0.*.* | head -1)" libweston-9.so.0
copy_soname "$(cd "$SYSROOT/lib" && ls libweston-desktop-9.so.0.*.* | head -1)" libweston-desktop-9.so.0
copy_file "$SYSROOT/lib/weston/$(cd "$SYSROOT/lib/weston" && ls libexec_weston.so.0.*.* | head -1)" /lib/libexec_weston.so.0
copy_file "$SYSROOT/lib/libweston-9/fbdev-backend.so" /lib/libweston-9/fbdev-backend.so
copy_file "$SYSROOT/lib/weston/desktop-shell.so" /lib/weston/desktop-shell.so
copy_file "$SYSROOT/lib/weston/kiosk-shell.so" /lib/weston/kiosk-shell.so

echo "[image] weston binaries"
copy_file "$SYSROOT/bin/weston" /weston
copy_file "$SYSROOT/bin/weston-simple-shm" /weston-simple-shm
if [ -f "$SYSROOT/bin/weston-terminal" ]; then
    copy_file "$SYSROOT/bin/weston-terminal" /weston-terminal
fi
if [ -f "$SYSROOT/libexec/weston-desktop-shell" ]; then
    copy_file "$SYSROOT/libexec/weston-desktop-shell" /libexec/weston-desktop-shell
fi
if [ -f "$SYSROOT/bin/a20-desktop-shell" ]; then
    copy_file "$SYSROOT/bin/a20-desktop-shell" /libexec/weston-desktop-shell
fi
if [ -f "$SYSROOT/bin/a20-input-method" ]; then
    copy_file "$SYSROOT/bin/a20-input-method" /libexec/weston-keyboard
fi
if [ -f "$SYSROOT/libexec/weston-keyboard" ]; then
    copy_file "$SYSROOT/libexec/weston-keyboard" /libexec/weston-keyboard
fi

echo "[image] weston data"
if [ -d "$SYSROOT/share/weston" ]; then
    (cd "$SYSROOT/share/weston" && find . -type f | sed "s|^\./||") | while read -r f; do
        copy_file "$SYSROOT/share/weston/$f" "/share/weston/$f"
    done
fi

echo "[image] Breeze cursor theme"
BREEZE_CURSOR=$USER_DIR/external/breeze/cursors/Breeze/Breeze
if [ -d "$BREEZE_CURSOR/cursors" ]; then
    copy_file "$BREEZE_CURSOR/index.theme" /share/icons/Breeze/index.theme
    mdel -i "$IMG" '::/share/icons/Breeze/cursors/*' >/dev/null 2>&1 || true
    BREEZE_CURSORS=(
        bottom_left_corner bottom_right_corner bottom_side grabbing left_ptr
        left_side right_side top_left_corner top_right_corner top_side xterm
        hand1 watch dnd-move dnd-copy dnd-none
    )
    for name in "${BREEZE_CURSORS[@]}"; do
        copy_file "$BREEZE_CURSOR/cursors/$name" \
            "/share/icons/Breeze/cursors/$name"
    done
fi

echo "[image] libinput quirks"
if [ -d "$SYSROOT/share/libinput" ]; then
    (cd "$SYSROOT/share/libinput" && find . -type f | sed "s|^\./||") | while read -r f; do
        copy_file "$SYSROOT/share/libinput/$f" "/share/libinput/$f"
    done
fi

echo "[image] xkeyboard-config data"
XKBC=$USER_DIR/external/xkeyboard-config
XKB_BUILD=${XKB_BUILD:-$USER_DIR/build/xkeyboard-config}
# Compiled rules (evdev etc.) are generated by a native meson build:
#   meson setup $XKB_BUILD $XKBC && ninja -C $XKB_BUILD
for d in keycodes types compat symbols geometry; do
    [ -d "$XKBC/$d" ] || continue
    (cd "$XKBC/$d" && find . -type f | sed "s|^\./||") | while read -r f; do
        copy_file "$XKBC/$d/$f" "/usr/share/X11/xkb/$d/$f"
    done
done
if [ -f "$XKB_BUILD/rules/evdev" ]; then
    for f in evdev evdev.lst evdev.xml base base.lst base.xml README; do
        [ -f "$XKB_BUILD/rules/$f" ] || continue
        copy_file "$XKB_BUILD/rules/$f" "/usr/share/X11/xkb/rules/$f"
    done
else
    echo "[image] WARNING: $XKB_BUILD/rules/evdev missing; xkb keymaps will not compile" >&2
fi

echo "[image] weston.ini"
WESTON_CONFIG='[core]
require-input=false
[shell]
background-color=0xff002244
[launcher]
icon=/bin/share/weston/terminal.png
path=/bin/weston-terminal
[launcher]
icon=/bin/share/weston/fullscreen.png
path=/bin/run-player.sh
'
printf '%s' "$WESTON_CONFIG" | \
    mcopy -o -i "$IMG" - ::/etc/xdg/weston/weston.ini 2>/dev/null || {
    mmd -D s -i "$IMG" ::/etc/xdg >/dev/null 2>&1 || true
    mmd -D s -i "$IMG" ::/etc/xdg/weston >/dev/null 2>&1 || true
    printf '%s' "$WESTON_CONFIG" | \
        mcopy -o -i "$IMG" - ::/etc/xdg/weston/weston.ini
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
export LIBINPUT_QUIRKS_DIR=/bin/share/libinput
export FONTCONFIG_FILE=/bin/etc/fonts/fonts.conf
export WESTON_DATA_DIR=/bin/share/weston
export XCURSOR_PATH=/bin/share/icons
export XCURSOR_THEME=Breeze
chmod 700 /tmp
mkdir -p /tmp/fontconfig
export WESTON_MODULE_MAP="fbdev-backend.so=/bin/lib/libweston-9/fbdev-backend.so;kiosk-shell.so=/bin/lib/weston/kiosk-shell.so;desktop-shell.so=/bin/lib/weston/desktop-shell.so;weston-desktop-shell=/bin/libexec/weston-desktop-shell;weston-keyboard=/bin/libexec/weston-keyboard"
exec weston --backend=fbdev-backend.so --seat=seat1 --shell=kiosk-shell.so "$@"
EOS
copy_file /tmp/opencode/run-weston-$$.sh /run-weston.sh

cat > /tmp/opencode/run-desktop-$$.sh <<'EOS'
#!/bin/sh
export XDG_RUNTIME_DIR=/tmp
export XKB_CONFIG_ROOT=/bin/usr/share/X11/xkb
export XDG_CONFIG_DIRS=/bin/etc/xdg
export LIBINPUT_QUIRKS_DIR=/bin/share/libinput
export FONTCONFIG_FILE=/bin/etc/fonts/fonts.conf
export WESTON_DATA_DIR=/bin/share/weston
export XCURSOR_PATH=/bin/share/icons
export XCURSOR_THEME=Breeze
chmod 700 /tmp
mkdir -p /tmp/fontconfig
export WESTON_MODULE_MAP="fbdev-backend.so=/bin/lib/libweston-9/fbdev-backend.so;kiosk-shell.so=/bin/lib/weston/kiosk-shell.so;desktop-shell.so=/bin/lib/weston/desktop-shell.so;weston-desktop-shell=/bin/libexec/weston-desktop-shell;weston-keyboard=/bin/libexec/weston-keyboard"
if [ -x /bin/libexec/weston-desktop-shell ]; then
    exec weston --backend=fbdev-backend.so --seat=seat1 --shell=desktop-shell.so "$@"
fi
echo "run-desktop: weston-desktop-shell is not installed" >&2
exit 1
EOS
copy_file /tmp/opencode/run-desktop-$$.sh /run-desktop.sh

cat > /tmp/opencode/run-terminal-$$.sh <<'EOS'
#!/bin/sh
export XDG_RUNTIME_DIR=/tmp
export XKB_CONFIG_ROOT=/bin/usr/share/X11/xkb
export XDG_CONFIG_DIRS=/bin/etc/xdg
export FONTCONFIG_FILE=/bin/etc/fonts/fonts.conf
export WESTON_DATA_DIR=/bin/share/weston
export XCURSOR_PATH=/bin/share/icons
export XCURSOR_THEME=Breeze
export SHELL=/bin/mksh
chmod 700 /tmp
mkdir -p /tmp/fontconfig
exec weston-terminal "$@"
EOS
copy_file /tmp/opencode/run-terminal-$$.sh /run-terminal.sh

cat > /tmp/opencode/run-player-$$.sh <<'EOS'
#!/bin/sh
export XDG_RUNTIME_DIR=/tmp
exec a20-player /bin/media/demo.mp4 "$@"
EOS
copy_file /tmp/opencode/run-player-$$.sh /run-player.sh
rm -f /tmp/opencode/run-weston-$$.sh /tmp/opencode/run-desktop-$$.sh \
    /tmp/opencode/run-terminal-$$.sh /tmp/opencode/run-player-$$.sh

echo "[image] done"
