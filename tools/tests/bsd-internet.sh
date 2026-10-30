#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
printf 'nameserver 10.0.2.3\n' > /etc/resolv.conf
echo INTERNET_BEGIN
if [ -x /usr/bin/bsd-net-probe ]; then
    /usr/bin/bsd-net-probe
    echo PROBE_RC $?
fi
cat /proc/net/status
cat /proc/net/config
/bin/busybox wget -T 15 -O /tmp/internet-http.html http://example.com/
echo INTERNET_HTTP_RC $?
/bin/busybox wc -c /tmp/internet-http.html
/bin/busybox grep -o 'Example Domain' /tmp/internet-http.html
/bin/busybox wget -T 15 -O /tmp/internet-https.html https://example.com/
echo INTERNET_HTTPS_RC $?
/bin/busybox wc -c /tmp/internet-https.html
/bin/busybox grep -o 'Example Domain' /tmp/internet-https.html
cat /proc/net/dev
cat /proc/net/route
echo INTERNET_DONE
sleep 600
