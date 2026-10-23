#!/bin/sh
# Wait for xfconf to answer before starting its clients.  Starting each
# component once keeps an application failure visible in the session log.
/usr/lib/xfce4/xfconf/xfconfd &
# The wait bounds a broken session, not a slow host.  Under TCG emulation
# (riscv64/aarch64/loongarch64 desktops) one xfconf-query round trip can
# outrun a 3 s budget for minutes, and giving up there costs the whole
# desktop: xfsettingsd, xfdesktop, xfce4-panel and thunar are all started
# below.  A healthy guest still returns on the first attempt.
i=0
until timeout 10 xfconf-query -c xsettings -p /Net/ThemeName >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 60 ]; then
        echo "a20: xfconf did not become ready" >&2
        exit 1
    fi
    sleep 1
done
# The actions plugin's built-in item list hides Restart and offers Suspend,
# Switch User and Lock Screen, none of which this session can perform (no
# logind, no greeter, no Wayland screensaver).  Publish exactly the power menu
# org.xfce.SessionManager implements, and only while plugin-10 is the actions
# plugin in the shipped panel layout.
if [ "$(xfconf-query -c xfce4-panel -p /plugins/plugin-10 2>/dev/null)" = actions ]; then
    xfconf-query -c xfce4-panel -p /plugins/plugin-10/items -n -a \
        -t string -s "+logout" -t string -s "+restart" -t string -s "+shutdown" \
        >/dev/null 2>&1 || \
        echo "a20: could not publish the actions menu item list" >&2
fi
/usr/lib/a20/start-input-method &
xfsettingsd &
xfdesktop &
xfce4-panel &
thunar --daemon &
wait
