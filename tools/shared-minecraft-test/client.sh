#!/bin/sh
set -eu
sleep 5
D=/run/a20-cluster-minecraft
mkdir -p "$D"
cat > "$D/node.conf" <<'EOF'
version 1
node 101112131415161718191a1b1c1d1e1f
instance 88000000000000000000000000000000
epoch 20261013
role worker
EOF
cat > "$D/apps.conf" <<EOF
A20APPS1
app cd000000000000000000000000000001 /usr/local/bin/app-exec-x86_64 62 64 1 1 2
require-feature 4
arg /bin/sh
arg /usr/local/bin/minecraft
arg --demo
arg --width
arg 854
arg --height
arg 480
env HOME=/root
env PATH=/usr/local/bin:/usr/bin:/bin
env WAYLAND_DISPLAY=$WAYLAND_DISPLAY
env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR
env JAVA_TOOL_OPTIONS=-XX:ActiveProcessorCount=2
env GAMEDIR=/root/.minecraft-cluster-owner
end
EOF
/usr/local/bin/cluster-supervisor-x86_64 "$D/node.conf" >"$D/supervisor.log" 2>&1 &
supervisor=$!
i=0
while ! grep -q 'CLUSTER_SUPERVISOR: ready' "$D/supervisor.log"; do
 i=$((i+1)); [ "$i" -lt 100 ] || exit 81; sleep 0.1
done
/usr/local/bin/appd-x86_64 --manifest "$D/apps.conf" &
owner=$!
sleep 1
printf 'MINECRAFT_OWNER: starting through appctl-run; CPUs='
grep -c '^processor' /proc/cpuinfo
# Record ordinary procfs thread state while this same client is running.
(
 while :; do
  pid=$(pgrep -x java | head -1)
  if [ -n "$pid" ]; then
   for stat in /proc/"$pid"/task/*/stat; do
    printf 'MINECRAFT_THREAD_STAT: root=%s ' "$pid"
    cat "$stat" 2>/dev/null || true
   done
  fi
  sleep 10
 done
) &
observer=$!
status=0
/usr/local/bin/appctl-run-x86_64 cd000000000000000000000000000001 20261013 30000 101112131415161718191a1b1c1d1e1f || status=$?
printf 'MINECRAFT_OWNER: appctl exit status=%s\n' "$status"
kill -TERM "$observer" "$owner" "$supervisor"
wait "$owner" || true
wait "$supervisor" || true
sync
printf '%s\n' "$status" > /run/a20-minecraft-result
pkill -TERM -x labwc
exit "$status"
