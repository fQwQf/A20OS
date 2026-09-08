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
spawn() {
    app=$1
    shift
    n=0
    while [ "$n" -lt "$MAX_TRIES" ]; do
        "$app" "$@" &
        pid=$!
        wait "$pid"
        n=$((n + 1))
        [ "$n" -lt "$MAX_TRIES" ] && sleep 1
    done
}

spawn xfsettingsd &
spawn xfdesktop &
spawn xfce4-panel &
thunar --daemon &
wait
