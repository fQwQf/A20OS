#!/bin/sh
# Quick real-browser gate inside the XFCE session.
sleep 30
echo BSD_BROWSER_BEGIN
cat /proc/net/status
python3 /usr/lib/a20/bsd_socket_loopback.py
echo BSD_BROWSER_SOCKET_LOOPBACK_RC $?
curl -4 -f --retry 2 --retry-all-errors --max-time 30 \
    https://example.org/ -o /tmp/bsd-browser-curl.html
echo BSD_BROWSER_CURL_RC $?
XFCE_WEB_DEFAULT_PROFILE=1 XFCE_WEB_LAUNCH=1 XFCE_WEB_QUICK=1 python3 -u /usr/lib/a20/xfce-firefox-webdriver.py
rc=$?
echo BSD_BROWSER_WEBDRIVER_RC $rc
python3 - <<'EOF'
try:
    data = open('/tmp/bsd-firefox.log', 'rb').read()[-12000:]
except OSError as error:
    data = repr(error).encode()
print('BSD_BROWSER_LOG_BEGIN')
print(data.decode(errors='replace'))
print('BSD_BROWSER_LOG_END')
EOF
if [ "$rc" -eq 0 ]; then echo BSD_BROWSER_PASS; fi
echo BSD_BROWSER_DONE
sleep 5
poweroff -f
sleep 30
