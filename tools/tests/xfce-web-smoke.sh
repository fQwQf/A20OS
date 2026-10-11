#!/bin/sh
# Guest-side test, launched inside the XFCE D-Bus/Wayland session.
# See docs/distro/xfce-x86_64-validation.md for image injection and screenshots.
set -eu
trap 'status=$?; if [ "$status" -ne 0 ]; then cat /tmp/xfce-firefox.log 2>/dev/null || :; echo "XFCE_WEB: FAIL status=$status"; fi' 0
sleep 30
xfconf-query -c a20-web-test -p /roundtrip -n -t string -s browser-ok
test "$(xfconf-query -c a20-web-test -p /roundtrip)" = browser-ok
xfconf-query -c a20-web-test -p /roundtrip -r
echo 'XFCE_WEB: configuration roundtrip PASS'
url=${XFCE_WEB_URL:-https://www.bing.com/}
curl -f --retry 2 --retry-all-errors --max-time 30 http://example.org/ -o /tmp/xfce-http.html
grep -q 'Example Domain' /tmp/xfce-http.html
echo 'XFCE_WEB: HTTP PASS'
# Keep certificate verification enabled; this also exercises DNS and TLS.
curl -f --retry 2 --retry-all-errors --max-time 30 https://example.org/ -o /tmp/xfce-https.html
grep -q 'Example Domain' /tmp/xfce-https.html
echo 'XFCE_WEB: HTTPS PASS'
firefox --marionette "$url" >/tmp/xfce-firefox.log 2>&1 &
python3 /usr/lib/a20/xfce-firefox-webdriver.py
pidof firefox
pidof xfdesktop xfce4-panel xfsettingsd xfconfd labwc
cat /tmp/xfce-firefox.log
echo 'XFCE_WEB: desktop and browser PASS (visual interaction checked separately)'
