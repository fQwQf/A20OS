#!/bin/sh
# Wait for xfconf to answer before starting its clients.  Starting each
# component once keeps an application failure visible in the session log.
/usr/lib/xfce4/xfconf/xfconfd &
i=0
until timeout 3 xfconf-query -c xsettings -p /Net/ThemeName >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 10 ]; then
        echo "a20: xfconf did not become ready" >&2
        exit 1
    fi
    sleep 0.5
done
xfsettingsd &
xfdesktop &
xfce4-panel &
thunar --daemon &
wait
