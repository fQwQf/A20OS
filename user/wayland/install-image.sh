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
MEDIA=${3:-}
DESKTOP=${4:-weston}

case "$DESKTOP" in
    weston|xfce) ;;
    *) echo "unsupported GUI desktop: $DESKTOP (expected weston or xfce)" >&2; exit 2 ;;
esac
BUILD=$USER_DIR/build/wayland
SYSROOT=$BUILD/$ARCH/sysroot
MUSL_SH=$BUILD/musl-$ARCH

[ -f "$IMG" ] || { echo "image not found: $IMG" >&2; exit 1; }
if [ -n "$MEDIA" ] && [ ! -f "$MEDIA" ]; then
    echo "media file not found: $MEDIA" >&2
    exit 1
fi

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

MEDIA_INSTALLED=0
if [ -f "$SYSROOT/bin/a20-player" ]; then
    echo "[image] FFmpeg + media player"
    copy_soname "$(cd "$SYSROOT/lib" && ls libavformat.so.61.*.* | head -1)" libavformat.so.61
    copy_soname "$(cd "$SYSROOT/lib" && ls libavcodec.so.61.*.* | head -1)" libavcodec.so.61
    copy_soname "$(cd "$SYSROOT/lib" && ls libswresample.so.5.*.* | head -1)" libswresample.so.5
    copy_soname "$(cd "$SYSROOT/lib" && ls libswscale.so.8.*.* | head -1)" libswscale.so.8
    copy_soname "$(cd "$SYSROOT/lib" && ls libavutil.so.59.*.* | head -1)" libavutil.so.59
    copy_file "$SYSROOT/bin/a20-player" /a20-player
    copy_file "$SYSROOT/bin/wayland-session" /wayland-session
    if [ -n "$MEDIA" ]; then
        copy_file "$MEDIA" /media/demo.mp4
        MEDIA_INSTALLED=1
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

if [ -f "$SYSROOT/lib/libEGL.so.1.0.0" ]; then
    echo "[image] mesa EGL/GLES + gbm"
    copy_file "$SYSROOT/lib/libEGL.so.1" /lib/libEGL.so.1
    copy_file "$SYSROOT/lib/libGLESv2.so.2" /lib/libGLESv2.so.2
    copy_file "$SYSROOT/lib/libGLESv1_CM.so.1" /lib/libGLESv1_CM.so.1
    copy_file "$SYSROOT/lib/libgbm.so.1" /lib/libgbm.so.1
    copy_file "$SYSROOT/lib/gbm/dri_gbm.so" /lib/gbm/dri_gbm.so
    copy_file "$SYSROOT/lib/libgallium-25.3.6.so" /lib/libgallium-25.3.6.so
    copy_file "$SYSROOT/lib/libstdc++.so.6" /lib/libstdc++.so.6
    copy_file "$SYSROOT/lib/libgcc_s.so.1" /lib/libgcc_s.so.1
    copy_file "$SYSROOT/lib/libdrm.so.2" /lib/libdrm.so.2
    copy_file "$SYSROOT/bin/egl_test" /egl_test
fi

if [ "$DESKTOP" = xfce ] && [ -f "$SYSROOT/bin/xfce4-session" ]; then
    echo "[image] XFCE desktop"
    printf 'a20os-xfce-machine-id-0000000000000001\n' | mcopy -o -i "$IMG" - ::/etc/machine-id
    # GTK/glib shared libs
    for lib in libglib-2.0.so.0 libgobject-2.0.so.0 libgio-2.0.so.0 \
               libgthread-2.0.so.0 libgmodule-2.0.so.0 \
               libatk-1.0.so.0 libgdk_pixbuf-2.0.so.0 libcairo.so.2 \
               libcairo-gobject.so.2 libpango-1.0.so.0 libpangocairo-1.0.so.0 \
               libpangoft2-1.0.so.0 libfribidi.so.0 libharfbuzz.so.0 \
               libgtk-3.so.0 libgdk-3.so.0 libgailutil-3.so.0 \
               libepoxy.so.0 libwayland-egl.so.1 \
                libxfce4util.so.7 libxfce4windowing-0.so.0 libxfconf-0.so.3 \
                libxfce4windowingui-0.so.0 libxfce4panel-2.0.so.4 \
                libxfce4ui-2.so.0 libexo-2.so.0 libgarcon-1.so.0 \
               libgarcon-gtk3-1.so.0 libgtk-layer-shell.so.0 \
               libjpeg.so.8.2.2 libdbus-1.so.3; do
        copy_file "$SYSROOT/lib/$lib" "/lib/$lib" 2>/dev/null || true
    done
    # XFCE binaries (FAT root is mounted at /bin in the guest)
    for b in xfce4-session xfce4-session-logout xfce4-session-settings \
             xfce4-panel xfdesktop xfdesktop-settings startxfce4 \
             xfsettingsd \
             xfconf-query xfce4-about xfce4-popup-applicationsmenu \
             xfce4-popup-windowmenu xfce4-popup-directorymenu; do
        copy_file "$SYSROOT/bin/$b" "/$b" 2>/dev/null || true
    done
    # xfconfd (xfce4-session reads the failsafe session config over D-Bus)
    if [ -x "$SYSROOT/lib/xfce4/xfconf/xfconfd" ]; then
        copy_file "$SYSROOT/lib/xfce4/xfconf/xfconfd" /lib/xfce4/xfconf/xfconfd
    fi
    # The panel binary does not contain its first-run layout or plugin
    # modules.  Omitting these files leaves xfce4-panel in the interactive
    # migration dialog forever, so install the vendor default and the small
    # built-in plugin set alongside the executable.
    if [ -f "$SYSROOT/etc/xdg/xfce4/panel/default.xml" ]; then
        copy_file "$SYSROOT/etc/xdg/xfce4/panel/default.xml" \
            /etc/xdg/xfce4/panel/default.xml
        # Seed xfconf as well.  The cross build's migration helper contains
        # build-host paths, while the channel file is architecture-neutral
        # and lets the panel start without any first-run UI.
        copy_file "$SYSROOT/etc/xdg/xfce4/panel/default.xml" \
            /etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml
    fi
    # Garcon resolves the Applications menu below XDG_CONFIG_DIRS.  Keep
    # this data file on the image so the panel's applicationsmenu plugin does
    # not open a startup error dialog when it first receives input.
    if [ -f "$SYSROOT/etc/xdg/menus/xfce-applications.menu" ]; then
        copy_file "$SYSROOT/etc/xdg/menus/xfce-applications.menu" \
            /etc/xdg/menus/xfce-applications.menu
    fi
    copy_file "$USER_DIR/wayland/xfce4-desktop.xml" \
        /etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml
    if [ -x "$SYSROOT/lib/xfce4/panel/migrate" ]; then
        copy_file "$SYSROOT/lib/xfce4/panel/migrate" /lib/xfce4/panel/migrate
    fi
    if [ -x "$SYSROOT/lib/xfce4/panel/wrapper-2.0" ]; then
        copy_file "$SYSROOT/lib/xfce4/panel/wrapper-2.0" \
            /lib/xfce4/panel/wrapper-2.0
    fi
    if [ -d "$SYSROOT/lib/xfce4/panel/plugins" ]; then
        while IFS= read -r plugin; do
            rel=${plugin#"$SYSROOT/lib/"}
            case "$plugin" in
                *.so) copy_file "$plugin" "/lib/$rel" ;;
            esac
        done < <(find "$SYSROOT/lib/xfce4/panel/plugins" -type f -name '*.so')
    fi
    if [ -d "$SYSROOT/share/xfce4/panel/plugins" ]; then
        while IFS= read -r desktop; do
            rel=${desktop#"$SYSROOT/share/"}
            copy_file "$desktop" "/share/$rel"
        done < <(find "$SYSROOT/share/xfce4/panel/plugins" -type f -name '*.desktop')
    fi
    if [ -d "$SYSROOT/share/backgrounds/xfce" ]; then
        while IFS= read -r background; do
            rel=${background#"$SYSROOT/share/"}
            copy_file "$background" "/share/$rel"
        done < <(find "$SYSROOT/share/backgrounds/xfce" -type f)
    fi
    if [ -f "$SYSROOT/share/pixmaps/xfdesktop/xfdesktop-fallback-icon.png" ]; then
        copy_file "$SYSROOT/share/pixmaps/xfdesktop/xfdesktop-fallback-icon.png" \
            /share/pixmaps/xfdesktop/xfdesktop-fallback-icon.png
    fi
    # xfce4-session failsafe config (FailsafeWayland starts the panel,
    # xfdesktop and Thunar under Wayland)
    if [ -f "$SYSROOT/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-session.xml" ]; then
        copy_file "$SYSROOT/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-session.xml" \
            /etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-session.xml
    fi
    # D-Bus daemon for the session bus
    if [ -x "$SYSROOT/bin/dbus-daemon" ]; then
        copy_file "$SYSROOT/bin/dbus-daemon" /dbus-daemon
    fi
    if [ -x "$SYSROOT/bin/dbus-launch" ]; then
        copy_file "$SYSROOT/bin/dbus-launch" /dbus-launch
    fi
    if [ -f "$SYSROOT/share/dbus-1/session.conf" ]; then
        copy_file "$SYSROOT/share/dbus-1/session.conf" /share/dbus-1/session.conf
    fi
    # Guest-local session bus config: dbus-daemon was cross-built with a host
    # absolute path to session.conf, so we ship a minimal config that listens
    # on the exact socket the XFCE components connect to.
    cat > /tmp/opencode/dbus-session-$$.conf <<'EOS'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <keep_umask/>
  <!-- Keep the session bus usable while the desktop is brought up. -->
  <listen>unix:path=/tmp/dbus-session</listen>
  <auth>ANONYMOUS</auth>
  <allow_anonymous/>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
  <limit name="max_incoming_bytes">1000000000</limit>
  <limit name="max_outgoing_bytes">1000000000</limit>
  <limit name="max_message_size">1000000000</limit>
  <limit name="max_connections_per_user">100000</limit>
</busconfig>
EOS
    copy_file /tmp/opencode/dbus-session-$$.conf /etc/dbus-1/session.conf
    rm -f /tmp/opencode/dbus-session-$$.conf
fi

if [ "$DESKTOP" = xfce ] && [ -x "$SYSROOT/bin/labwc" ]; then
    echo "[image] labwc compositor (wlroots)"
    copy_file "$SYSROOT/bin/labwc" /labwc
    for lib in libwlroots-0.19.so libdisplay-info.so.5 libxml2.so.2 \
               libseat.so.1; do
        copy_file "$SYSROOT/lib/$lib" "/lib/$lib" 2>/dev/null || true
    done
fi
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
elif [ -f "$SYSROOT/bin/a20-desktop-shell" ]; then
    copy_file "$SYSROOT/bin/a20-desktop-shell" /libexec/weston-desktop-shell
fi
if [ -f "$SYSROOT/libexec/weston-keyboard" ]; then
    copy_file "$SYSROOT/libexec/weston-keyboard" /libexec/weston-keyboard
elif [ -f "$SYSROOT/bin/a20-input-method" ]; then
    copy_file "$SYSROOT/bin/a20-input-method" /libexec/weston-keyboard
fi

echo "[image] weston data"
if [ -d "$SYSROOT/share/weston" ]; then
    (cd "$SYSROOT/share/weston" && find . -type f | sed "s|^\./||") | while read -r f; do
        copy_file "$SYSROOT/share/weston/$f" "/share/weston/$f"
    done
fi

echo "[image] Breeze cursor theme"
BREEZE_CURSOR=$USER_DIR/external/gui/breeze/cursors/Breeze/Breeze
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
XKBC=$USER_DIR/external/gui/xkeyboard-config
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
'
if [ "$MEDIA_INSTALLED" = 1 ]; then
    WESTON_CONFIG+='[launcher]
icon=/bin/share/weston/icon_editor.png
path=/bin/run-player.sh
'
fi
printf '%s' "$WESTON_CONFIG" | \
    mcopy -o -i "$IMG" - ::/etc/xdg/weston/weston.ini 2>/dev/null || {
    mmd -D s -i "$IMG" ::/etc/xdg >/dev/null 2>&1 || true
    mmd -D s -i "$IMG" ::/etc/xdg/weston >/dev/null 2>&1 || true
    printf '%s' "$WESTON_CONFIG" | \
        mcopy -o -i "$IMG" - ::/etc/xdg/weston/weston.ini
}

# The init process uses an image marker to choose the default compositor.
printf '%s\n' "$DESKTOP" | mcopy -o -i "$IMG" - ::/etc/a20-gui-desktop
printf '%s\n' "$DESKTOP" | mcopy -o -i "$IMG" - ::/etc/a20-gui-desktop-$DESKTOP
if [ "$DESKTOP" = weston ]; then
    printf '1\n' | mcopy -o -i "$IMG" - ::/etc/weston
else
    printf '1\n' | mcopy -o -i "$IMG" - ::/etc/xfce
fi

echo "[image] run-weston.sh"
# Launcher recipe (FAT32 mounts at /bin at runtime):
#  - modules live in /bin/lib/libweston-9 and /bin/lib/weston
#  - xkb data in /bin/usr/share/X11/xkb (XKB_CONFIG_ROOT)
#  - seat1 avoids the VT setup in launcher-direct (no VTs on A20OS)
cat > /tmp/opencode/run-weston-$$.sh <<'EOS'
#!/bin/sh
XKB_CONFIG_ROOT=/bin/usr/share/X11/xkb
LIBINPUT_QUIRKS_DIR=/bin/share/libinput
FONTCONFIG_FILE=/bin/etc/fonts/fonts.conf
WESTON_DATA_DIR=/bin/share/weston
XCURSOR_PATH=/bin/share/icons
XCURSOR_THEME=Breeze
unset WESTON_LIBINPUT_UDEV
export WESTON_LIBINPUT_DEVICE=/dev/input/event0
mkdir -p /tmp/fontconfig
export WESTON_MODULE_MAP="fbdev-backend.so=/bin/lib/libweston-9/fbdev-backend.so;kiosk-shell.so=/bin/lib/weston/kiosk-shell.so;desktop-shell.so=/bin/lib/weston/desktop-shell.so;weston-desktop-shell=/bin/libexec/weston-desktop-shell;weston-keyboard=/bin/libexec/weston-keyboard"
exec weston --backend=fbdev-backend.so --device=/dev/fb0 --seat=seat1 --shell=kiosk-shell.so "$@"
EOS
chmod 755 /tmp/opencode/run-weston-$$.sh
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
unset WESTON_LIBINPUT_UDEV
export WESTON_LIBINPUT_DEVICE=/dev/input/event0
chmod 700 /tmp
mkdir -p /tmp/fontconfig
export WESTON_MODULE_MAP="fbdev-backend.so=/bin/lib/libweston-9/fbdev-backend.so;kiosk-shell.so=/bin/lib/weston/kiosk-shell.so;desktop-shell.so=/bin/lib/weston/desktop-shell.so;weston-desktop-shell=/bin/libexec/weston-desktop-shell;weston-keyboard=/bin/libexec/weston-keyboard"
if [ -x /bin/libexec/weston-desktop-shell ]; then
    exec weston --backend=fbdev-backend.so --device=/dev/fb0 --seat=seat1 --shell=desktop-shell.so "$@"
fi
echo "run-desktop: weston-desktop-shell is not installed" >&2
exit 1
EOS
chmod 755 /tmp/opencode/run-desktop-$$.sh
copy_file /tmp/opencode/run-desktop-$$.sh /run-desktop.sh

cat > /tmp/opencode/run-xfce-$$.sh <<'EOS'
#!/bin/sh
echo "[xfce] script entered" >&2
# Keep the runtime path minimal.  FAT32 images do not reliably support the
# shell's environment-assignment and pathname-test builtins used below.
echo "[xfce] starting labwc compositor" >&2
echo "[xfce] starting weston compositor" >&2
/bin/weston --backend=fbdev-backend.so --seat=seat1 --shell=desktop-shell.so >&2 &
/bin/sleep 2
echo "[xfce] starting XFCE panel and desktop" >&2
/bin/xfce4-panel >&2 &
/bin/xfdesktop >&2 &
echo "AUTOSTART_DONE" >&2
wait
# XFCE Wayland session: session bus + compositor + XFCE session.
export XDG_RUNTIME_DIR=/tmp
export XKB_CONFIG_ROOT=/bin/usr/share/X11/xkb
export XDG_CONFIG_DIRS=/bin/etc/xdg
export LIBINPUT_QUIRKS_DIR=/bin/share/libinput
export FONTCONFIG_FILE=/bin/etc/fonts/fonts.conf
export WESTON_DATA_DIR=/bin/share/weston
export XCURSOR_PATH=/bin/share/icons
export XCURSOR_THEME=Breeze
echo "[xfce] basic env ready" >&2
echo "[xfce] using existing temp root" >&2
export WESTON_MODULE_MAP="fbdev-backend.so=/bin/lib/libweston-9/fbdev-backend.so;kiosk-shell.so=/bin/lib/weston/kiosk-shell.so;desktop-shell.so=/bin/lib/weston/desktop-shell.so;weston-desktop-shell=/bin/libexec/weston-desktop-shell;weston-keyboard=/bin/libexec/weston-keyboard"

# Session bus first (xfconf and the panel need D-Bus).  Start dbus-daemon
# directly with an explicit address; dbus-launch was built with host
# absolute paths baked in.
echo "[xfce] starting session bus" >&2
if [ -f /bin/dbus-daemon ]; then
    /bin/dbus-daemon --config-file=/bin/etc/dbus-1/session.conf --fork \
        --print-address >&2
    echo "[xfce] session bus started" >&2
fi
echo "[xfce] session bus wait complete" >&2

# xfconfd serves the xfce4-session / xfce4-panel / xfdesktop configuration
# over the session bus.  XDG_CONFIG_DIRS must point at the FAT configs
# (/bin = FAT root at runtime).
# Start the compositor and the XFCE session.  labwc is the XFCE 4.20
# reference compositor: it implements wlr-layer-shell etc. natively and
# starts the session command (-s) after coming up.
if [ -f /bin/labwc ]; then
    echo "[xfce] starting labwc compositor" >&2
    # labwc/wlroots: force the DRM backend.  The WAYLAND_DISPLAY above
    # (needed by the XFCE clients) would otherwise make wlroots pick the
    # wayland backend and fail.  A20OS exposes virtio-gpu as /dev/dri/card0.
    # A20OS has no udev; point wlroots at the virtio-gpu DRM node directly.
    # libseat builtin forks an embedded seatd server; without a VT subsystem
    # A20OS cannot bind the seat to a VT, so create a non-VT-bound seat.
    # Start labwc first, then run the XFCE components from this script once
    # its Wayland socket exists.  This avoids relying on labwc's optional
    # autostart-file feature, which is not enabled in this build.
    /bin/labwc >&2 &
    LABWC_PID=$!
    echo "[xfce] starting panel" >&2
    /bin/xfce4-panel >&2 &
    /bin/xfdesktop >&2 &
    /bin/xfsettingsd >&2 &
    echo "AUTOSTART_DONE" >&2
    wait "$LABWC_PID"
    exit $?
fi
# Fallback: Weston (no wlr-layer-shell) for images without labwc.
/bin/weston --backend=fbdev-backend.so --seat=seat1 --shell=desktop-shell.so &
WESTON_PID=$!
i=0
while [ $i -lt 300 ]; do
    [ -S /tmp/wayland-0 ] && break
    i=$((i+1))
    /bin/sleep 0.1
done
exec /bin/xfce4-session
EOS
chmod 755 /tmp/opencode/run-xfce-$$.sh
copy_file /tmp/opencode/run-xfce-$$.sh /run-xfce.sh

# Replace the compatibility recipe above with the actual GUI entrypoint.  A
# standalone file avoids mksh continuing through the legacy fallback body.
cat > /tmp/opencode/run-xfce-clean-$$.sh <<'EOS'
#!/bin/sh
echo "[xfce] script entered" >&2
exec /bin/wayland-session
echo "AUTOSTART_DONE" >&2
wait
EOS
chmod 755 /tmp/opencode/run-xfce-clean-$$.sh
copy_file /tmp/opencode/run-xfce-clean-$$.sh /run-xfce.sh
rm -f /tmp/opencode/run-xfce-clean-$$.sh

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
mkdir -p /tmp/fontconfig
exec weston-terminal "$@"
EOS
chmod 755 /tmp/opencode/run-terminal-$$.sh
copy_file /tmp/opencode/run-terminal-$$.sh /run-terminal.sh

cat > /tmp/opencode/run-player-$$.sh <<'EOS'
#!/bin/sh
export XDG_RUNTIME_DIR=/tmp
if [ "$#" -eq 0 ]; then
    if [ ! -f /bin/media/demo.mp4 ]; then
        echo "usage: run-player.sh FILE.mp4" >&2
        exit 2
    fi
    set -- /bin/media/demo.mp4
fi
exec a20-player "$@"
EOS
chmod 755 /tmp/opencode/run-player-$$.sh
copy_file /tmp/opencode/run-player-$$.sh /run-player.sh
rm -f /tmp/opencode/run-weston-$$.sh /tmp/opencode/run-desktop-$$.sh \
    /tmp/opencode/run-terminal-$$.sh /tmp/opencode/run-player-$$.sh

echo "[image] done"
