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
xfsettingsd &
xfdesktop &
xfce4-panel &
thunar --daemon &
wait
