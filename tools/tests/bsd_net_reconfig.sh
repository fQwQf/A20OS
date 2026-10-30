#!/bin/sh
# Guest-side dynamic IPv4/MTU transaction check for the BSD backend.
echo BSD_RECONFIG_BEGIN
ip link set dev a20n0 mtu 1400
echo BSD_RECONFIG_MTU_1400_RC $?
ip link show dev a20n0
ip addr replace 10.30.0.2/24 dev a20n0
echo BSD_RECONFIG_ADDR_ALT_RC $?
ip addr show dev a20n0
ip addr replace 10.0.2.15/24 dev a20n0
echo BSD_RECONFIG_ADDR_RESTORE_RC $?
ip addr show dev a20n0
ip link set dev a20n0 mtu 1500
echo BSD_RECONFIG_MTU_1500_RC $?
ip route del default
echo BSD_RECONFIG_ROUTE_DEL_RC $?
ip route add default via 10.0.2.2 dev a20n0
echo BSD_RECONFIG_ROUTE_ADD_RC $?
ip route show
curl -4 -f --max-time 30 https://example.org/ -o /tmp/bsd-reconfig.html
echo BSD_RECONFIG_HTTPS_RC $?
grep -q 'Example Domain' /tmp/bsd-reconfig.html
echo BSD_RECONFIG_CONTENT $?
echo BSD_RECONFIG_DONE
