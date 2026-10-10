#!/bin/sh
# Guest-side test, launched inside the XFCE D-Bus/Wayland session.
# See docs/distro/xfce-x86_64-validation.md for image injection and screenshots.
set -eu
trap 'status=$?; if [ "$status" -ne 0 ]; then echo "XFCE_WEB: FAIL status=$status"; fi' 0
sleep 30
url=${XFCE_WEB_URL:-https://example.org/}
curl -f --retry 2 --max-time 30 http://example.org/ -o /tmp/xfce-http.html
grep -q 'Example Domain' /tmp/xfce-http.html
echo 'XFCE_WEB: HTTP PASS'
# Keep certificate verification enabled; this also exercises DNS and TLS.
curl -f --retry 2 --max-time 30 "$url" -o /tmp/xfce-https.html
grep -q 'Example Domain' /tmp/xfce-https.html
echo 'XFCE_WEB: HTTPS PASS'
netsurf "$url" >/tmp/xfce-netsurf.log 2>&1 &
sleep 60
pidof netsurf netsurf-gtk3
pidof xfdesktop xfce4-panel xfsettingsd xfconfd labwc
xfconf-query -c xfce4-panel -p /panels/panel-1/position
cat /tmp/xfce-netsurf.log
echo 'XFCE_WEB: PASS (verify the rendered browser window in a screenshot)'
