#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
echo BSD_INBOUND_BEGIN
cat /proc/net/status
/usr/bin/net-microbench server tcp 39051 >/tmp/bsd-inbound-tcp.log 2>&1 &
sleep 1
echo BSD_INBOUND_TCP_READY
sleep 30
/usr/bin/net-microbench server udp 39052 >/tmp/bsd-inbound-udp.log 2>&1 &
sleep 1
echo BSD_INBOUND_UDP_READY
sleep 30
cat /proc/net/dev
cat /tmp/bsd-inbound-tcp.log
cat /tmp/bsd-inbound-udp.log
echo BSD_INBOUND_DONE
sleep 5
poweroff -f
sleep 30
