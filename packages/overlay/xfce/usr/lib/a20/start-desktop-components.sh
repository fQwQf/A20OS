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

xfsettingsd &
xfdesktop &
xfce4-panel &
thunar --daemon &
