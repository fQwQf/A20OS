#!/bin/sh
# Guest-only Firefox startup probe; runs in the real XFCE session.
sleep 20
echo BSD_FFPROBE_WRAPPER_BEGIN
cat /proc/net/status
python3 /usr/lib/a20/bsd_firefox_probe.py
echo BSD_FFPROBE_WRAPPER_RC $?
echo BSD_FFPROBE_WRAPPER_DONE
sleep 5
poweroff -f
sleep 30
