#!/bin/sh
# Ordered XFCE bring-up: xfconfd must own org.xfce.Xfconf before any client
# starts.  Under TCG the D-Bus activation race otherwise loses the settings
# service and the panel renders with its built-in empty default.
/usr/lib/xfce4/xfconf/xfconfd &
i=0
while [ "$i" -lt 150 ]; do
    if dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
        org.freedesktop.DBus.NameHasOwner string:org.xfce.Xfconf 2>/dev/null | grep -q true; then
        break
    fi
    sleep 0.2
    i=$((i + 1))
done

# A GTK component can die once at startup (theme CSS tree walk).  Respawn it
# a bounded number of times so a single transient crash does not leave the
# desktop headless; give up after MAX_TRIES consecutive fast deaths.
MAX_TRIES=5
FAST_DEATH_SECS=10
spawn() {
    app=$1
    shift
    fast_deaths=0
    while [ "$fast_deaths" -lt "$MAX_TRIES" ]; do
        started=$(date +%s)
        "$app" "$@" &
        pid=$!
        wait "$pid"
        if [ "$(( $(date +%s) - started ))" -lt "$FAST_DEATH_SECS" ]; then
            fast_deaths=$((fast_deaths + 1))
        else
            fast_deaths=0
        fi
        [ "$fast_deaths" -lt "$MAX_TRIES" ] && sleep 1
    done
    echo "a20: $app gave up after $MAX_TRIES fast deaths" >&2
}

spawn xfsettingsd &
spawn xfdesktop &
spawn xfce4-panel &
thunar --daemon &

# Wallpaper.  xfdesktop 4.20.1 never paints a Wayland backdrop on A20OS (see
# docs/distro/known-issues.md), so a compositor-level background tool draws
# it on the wlr-layer-shell background layer instead.
if command -v swaybg >/dev/null 2>&1; then
    swaybg -i /usr/share/backgrounds/xfce/xfce-flower.svg -m fill &
fi

wait
