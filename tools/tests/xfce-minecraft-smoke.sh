#!/bin/sh
# Run inside the desktop; manually play, save to title, then Quit Game.
# Reboot the same writable fixture and repeat to validate saved world reload.
set -eu
sleep 35
export GAMEDIR=${GAMEDIR:-$HOME/.minecraft-qa}
world="$GAMEDIR/saves/Demo_World/level.dat"
echo "MINECRAFT_QA: START gameDir=$GAMEDIR renderer=$(/usr/lib/a20/select-renderer)"
java -XshowSettings:vm -version
if [ -e "$world" ]; then
    gzip -t "$world"
    sha256sum "$world"
    echo 'MINECRAFT_QA: existing world compressed data PASS'
fi
status=0
minecraft --demo --width 854 --height 480 || status=$?
echo "MINECRAFT_QA: client exited status=$status"
[ "$status" -eq 0 ]
gzip -t "$world"
sha256sum "$world"
sync
echo 'MINECRAFT_QA: save+normal-exit+sync PASS (reboot reload still required)'
