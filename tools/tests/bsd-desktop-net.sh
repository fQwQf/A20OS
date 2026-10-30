#!/bin/sh
# Runs inside the real XFCE session. Keep TLS certificate verification on.
sleep 20
echo BSD_DESKTOP_NET_BEGIN
cat /proc/net/status
cat /proc/net/config
python3 /usr/lib/a20/bsd_socket_loopback.py
echo BSD_DESKTOP_SOCKET_LOOPBACK_RC $?
sh /usr/lib/a20/bsd_net_reconfig.sh
curl -4 -f --retry 2 --retry-all-errors --max-time 30 \
    http://example.org/ -o /tmp/bsd-desktop-http.html
echo BSD_DESKTOP_HTTP_RC $?
grep -q 'Example Domain' /tmp/bsd-desktop-http.html
echo BSD_DESKTOP_HTTP_CONTENT $?
curl -4 -f --retry 2 --retry-all-errors --max-time 30 \
    https://example.org/ -o /tmp/bsd-desktop-https.html
echo BSD_DESKTOP_HTTPS_RC $?
grep -q 'Example Domain' /tmp/bsd-desktop-https.html
echo BSD_DESKTOP_HTTPS_CONTENT $?
if grep -q 'Example Domain' /tmp/bsd-desktop-http.html &&
   grep -q 'Example Domain' /tmp/bsd-desktop-https.html; then
    echo BSD_DESKTOP_NET_PASS
fi
cat /proc/net/dev
echo BSD_DESKTOP_NET_DONE
sleep 5
poweroff -f
sleep 30
