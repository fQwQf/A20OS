#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
n=0
while ! pgrep -x labwc >/dev/null || ! pgrep -x xfce4-panel >/dev/null; do
    n=$((n+1))
    if [ "$n" -ge 90 ]; then
        echo 'A20_DESKTOP_SUPERVISION: FAIL desktop session absent'
        cat /var/log/a20/desktop-startup.log
        poweroff
        exit 1
    fi
    sleep 1
done
sleep 10
echo 'A20_DESKTOP_SUPERVISION: READY labwc and xfce4-panel'
ps -ef
/bin/svcmgr-aarch64 &
sleep 2
if ! test -d /ufs || ! echo A20_UFSD_ROUNDTRIP > /ufs/closeout.txt || ! test "$(cat /ufs/closeout.txt)" = A20_UFSD_ROUNDTRIP; then
    echo 'A20_DESKTOP_SUPERVISION: FAIL UFSD mount and I/O'
    poweroff
    exit 1
fi
rm /ufs/closeout.txt
echo 'A20_DESKTOP_SUPERVISION: UFSD_IO_PASS'
cat /proc/mounts
n=0
while [ "$n" -lt 3 ]; do
    /bin/native-registry-aarch64 || { echo 'A20_DESKTOP_SUPERVISION: FAIL registry'; poweroff; exit 1; }
    n=$((n+1))
done
rc-service a20-desktop status || { echo 'A20_DESKTOP_SUPERVISION: FAIL desktop exited'; poweroff; exit 1; }
# Retire the filesystem while its server is still alive. OpenRC's final
# KILL must not precede the user's filesystem shutdown protocol.
sync
umount /ufs || { echo 'A20_DESKTOP_SUPERVISION: FAIL UFSD unmount'; poweroff; exit 1; }
sleep 1
echo 'A20_DESKTOP_SUPERVISION: UFSD_UNMOUNT_PASS'
echo 'A20_DESKTOP_SUPERVISION: PASS cycles=3'
sleep 2
sync
poweroff
