#!/bin/sh
set -eu
export HOME=/root PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
mkdir -p /root/.config/labwc /run
cp /a20-cluster-payload/java /usr/bin/java
cp /a20-cluster-payload/minecraft /usr/local/bin/minecraft
cp /a20-cluster-payload/*-x86_64 /usr/local/bin/
printf '%s\n' '/bin/sh /a20-cluster-payload/client.sh >/dev/console 2>&1 &' > /root/.config/labwc/autostart
# Reuse the installed desktop's real service and compositor startup.
/bin/sh /sbin/init
[ -f /run/a20-minecraft-result ]
exit "$(cat /run/a20-minecraft-result)"
