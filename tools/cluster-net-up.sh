#!/usr/bin/env bash
# cluster-net-up.sh -- bring up the two cluster demo instances and verify
# bidirectional ICMP ping across QEMU's socket-UDP tunnel.
#
# This is the WB2 acceptance environment for the cluster UDP transport
# (docs/cluster/04-transports.md §3): instances/qemu-riscv64-cluster-a.toml
# (10.0.3.2) and -b.toml (10.0.3.3) share one L2 segment, each node binds its
# own host UDP port, and the script proves A ping B and B ping A with the
# guests' own `ping` command (user/cmds/net/ping.c).
#
# Usage:
#   tools/cluster-net-up.sh [--skip-build] [--keep]
#
#   --skip-build  assume the dev build is already in place (A20_CLUSTER_SKIP_BUILD=1)
#   --keep        leave both guests running after the verdict, for manual
#                 inspection (A20_CLUSTER_KEEP=1); connect with
#                 `tools/a20 run` REPLACED by attaching to the FIFOs -- see
#                 docs/cluster/02-udp-demo.md
#
# The script is idempotent: a rerun stops QEMU processes tagged
# a20os-cluster-{a,b} from a previous run first, re-checks the host UDP ports
# are bindable, rebuilds only if the kernel or image is missing, and re-runs
# the ping proof from scratch.  Every failure exits non-zero with the log tail
# that explains it.
#
# Knobs: A20_CLUSTER_BOOT_TIMEOUT (default 240 s, TCG cold boot), 
# A20_CLUSTER_PING_TIMEOUT (default 60 s, per ping attempt).

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="$ROOT/.kernel-build/cluster"
INST_A=qemu-riscv64-cluster-a
INST_B=qemu-riscv64-cluster-b
TAG_A=a20os-cluster-a
TAG_B=a20os-cluster-b
GUEST_IP_A=10.0.3.2
GUEST_IP_B=10.0.3.3
# Host ports each node binds for the L2 tunnel (see the instance manifests).
# NOT the guest cluster UDP port 44020+idx from 04-transports.md §3 -- those
# live inside the guests and need no host-side reservation.
PORT_A=44121
PORT_B=44122
BOOT_TIMEOUT="${A20_CLUSTER_BOOT_TIMEOUT:-240}"
PING_TIMEOUT="${A20_CLUSTER_PING_TIMEOUT:-60}"

SKIP_BUILD=0
KEEP=0
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
        --keep)       KEEP=1 ;;
        *) printf 'cluster-net-up: unknown option %s\n' "$arg" >&2; exit 2 ;;
    esac
done
SKIP_BUILD="${A20_CLUSTER_SKIP_BUILD:-$SKIP_BUILD}"
KEEP="${A20_CLUSTER_KEEP:-$KEEP}"

die() { printf 'cluster-net-up: %s\n' "$*" >&2; exit 1; }
# stderr: this script's stdout is data (captured function output), diagnostics
# are not -- a log line leaking into `$(with_private_image ...)` would end up
# eval'd as part of the QEMU command line.
log() { printf '[cluster-net-up] %s\n' "$*" >&2; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1 (install it or fix PATH)"; }

# ---------------------------------------------------------------- self-checks

for c in make python3 pgrep; do need_cmd "$c"; done
[[ -x "$ROOT/tools/a20" ]] || die "$ROOT/tools/a20 is missing or not executable"
for inst in "$INST_A" "$INST_B"; do
    [[ -f "$ROOT/instances/$inst.toml" ]] || die "instances/$inst.toml not found"
done

# stop_stale TAG: kill leftover QEMU processes tagged with TAG from a previous
# run -- only processes whose /proc/<pid>/comm says qemu-system-*, so an
# unrelated process merely mentioning the tag is never touched.
stop_stale() {
    local tag=$1 pid
    local pids
    pids="$(pgrep -f -- "$tag" 2>/dev/null || true)"
    [[ -z "$pids" ]] && return 0
    for pid in $pids; do
        [[ "$(cat "/proc/$pid/comm" 2>/dev/null)" == qemu-system-* ]] || continue
        log "stopping stale guest pid $pid ($tag)"
        kill "$pid" 2>/dev/null || true
    done
    local i=0
    while (( i < 20 )); do
        pgrep -f -- "$tag" >/dev/null 2>&1 || return 0
        sleep 0.5
        i=$((i + 1))
    done
    for pid in $pids; do
        [[ "$(cat "/proc/$pid/comm" 2>/dev/null)" == qemu-system-* ]] || continue
        kill -KILL "$pid" 2>/dev/null || true
    done
}

stop_stale "$TAG_A"
stop_stale "$TAG_B"

# A port held by anything else would make QEMU fail deep into the boot; check
# both tunnel ports are bindable right now.
port_free() {  # addr port proto
    python3 - "$1" "$2" "$3" <<'PYEOF'
import socket, sys
addr, port, proto = sys.argv[1], int(sys.argv[2]), sys.argv[3]
kind = socket.SOCK_DGRAM if proto == "udp" else socket.SOCK_STREAM
with socket.socket(socket.AF_INET, kind) as s:
    try:
        s.bind((addr, port))
    except OSError:
        sys.exit(1)
PYEOF
}
for port in "$PORT_A" "$PORT_B"; do
    port_free 127.0.0.1 "$port" udp || \
        die "host UDP port 127.0.0.1:$port is held by another process; free it (or retune the [net].backend ports in the cluster instance manifests)"
done

# ---------------------------------------------------------------------- build

# Both cluster instances derive the same build variables (ARCH/BOARD/ABI/
# profile), so they share one BUILD_DIR: building via node A serves both.
if [[ "$SKIP_BUILD" != 1 ]]; then
    log "building $INST_A (dev-build; node B shares the same build)"
    (cd "$ROOT" && tools/a20 build "$INST_A") || die "build failed (see make output above)"
else
    log "skipping build (--skip-build)"
fi

# ------------------------------------------------------------------- launch

mkdir -p "$LOG_DIR"
FIFO_A="$LOG_DIR/${TAG_A}.in"
FIFO_B="$LOG_DIR/${TAG_B}.in"
LOG_A="$LOG_DIR/${TAG_A}.log"
LOG_B="$LOG_DIR/${TAG_B}.log"
rm -f "$FIFO_A" "$FIFO_B" "$LOG_A" "$LOG_B"
mkfifo "$FIFO_A" "$FIFO_B"

# argv_for INSTANCE: print the QEMU command line make derives for the
# instance, and record it verbatim under .kernel-build/cluster/ (the recorded
# launch parameters).  The line is shell-syntax by contract (tools/run-targets.mk
# _qemu_argv); eval turns it into an argv vector, single-quoted -append value
# included.
argv_for() {
    local inst=$1 vars line
    vars="$(cd "$ROOT" && tools/a20 show-vars "$inst" 2>/dev/null)" \
        || die "tools/a20 show-vars $inst failed"
    line="$(cd "$ROOT" && make $vars _qemu_argv 2>/dev/null)" \
        || die "make _qemu_argv failed for $inst"
    [[ "$line" == *"qemu-system-"* ]] || die "make _qemu_argv printed no QEMU command line for $inst"
    printf '%s\n' "$line"
}

LINE_A="$(argv_for "$INST_A")"
LINE_B="$(argv_for "$INST_B")"

# with_private_image LINE TAG: give the instance its own writable rootfs.
# The two instances share one BUILD_DIR, so make's argv points both at the
# same fat32.img -- and QEMU holds a write lock on a raw image for the
# lifetime of the guest, so the second instance dies before boot with
# "Failed to get write lock".  A per-node copy keeps the shared build
# artifacts intact and lets both guests write.
with_private_image() {  # line tag
    local line=$1 tag=$2 tok src="" dst
    for tok in $line; do
        case "$tok" in
            file=*fat32.img,*) src="${tok#file=}"; src="${src%%,*}" ;;
        esac
    done
    [[ -n "$src" ]] || die "no fat32 drive found in the QEMU command line"
    [[ -f "$ROOT/$src" ]] || die "rootfs image $ROOT/$src not found (build first or drop --skip-build)"
    dst="$LOG_DIR/fat32-$tag.img"
    log "cloning $src -> $dst (per-instance writable rootfs)"
    cp -f "$ROOT/$src" "$dst" || die "rootfs image copy failed (disk full?)"
    printf '%s\n' "${line/file=$src,/file=$dst,}"
}
LINE_A="$(with_private_image "$LINE_A" a)"
LINE_B="$(with_private_image "$LINE_B" b)"

eval "ARGV_A=($LINE_A)"
eval "ARGV_B=($LINE_B)"
# Record the exact launch parameters: make's derivation with the per-node
# image rewrite and the -name tag already applied.  %q keeps it re-runnable.
printf '%q ' "${ARGV_A[@]}" -name "$TAG_A" > "$LOG_DIR/$INST_A.cmdline"
printf '\n' >> "$LOG_DIR/$INST_A.cmdline"
printf '%q ' "${ARGV_B[@]}" -name "$TAG_B" > "$LOG_DIR/$INST_B.cmdline"
printf '\n' >> "$LOG_DIR/$INST_B.cmdline"
# -name tags the process for idempotent reruns (stop_stale above).
"${ARGV_A[@]}" -name "$TAG_A" < "$FIFO_A" > "$LOG_A" 2>&1 &
PID_A=$!
"${ARGV_B[@]}" -name "$TAG_B" < "$FIFO_B" > "$LOG_B" 2>&1 &
PID_B=$!
log "started guests: pid $PID_A ($TAG_A), pid $PID_B ($TAG_B)"
printf '%s\n' "$PID_A" > "$LOG_DIR/$TAG_A.pid"
printf '%s\n' "$PID_B" > "$LOG_DIR/$TAG_B.pid"

cleanup() {
    local status=$?
    exec 50>&- 51>&- 2>/dev/null || true
    if [[ "$KEEP" == 1 || "$KEEP" == true ]]; then
        log "--keep: leaving guests running (pids $(cat "$LOG_DIR/$TAG_A.pid" 2>/dev/null) / $(cat "$LOG_DIR/$TAG_B.pid" 2>/dev/null)); kill them or rerun this script"
        return 0
    fi
    [[ -n "${PID_A:-}" ]] && kill "$PID_A" 2>/dev/null || true
    [[ -n "${PID_B:-}" ]] && kill "$PID_B" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f "$FIFO_A" "$FIFO_B"
    return "$status"
}
trap cleanup EXIT

# Hold the write ends of both FIFOs so QEMU never sees EOF on its serial
# input.  QEMU opened the read ends when it started.
exec 50>"$FIFO_A"
exec 51>"$FIFO_B"

log_fail_tail() {  # logfile lines...
    local file=$1
    shift
    printf '%s\n' "--- tail of $file ---" >&2
    tail -n "${1:-25}" "$file" 2>/dev/null >&2 || true
}

alive() { kill -0 "$1" 2>/dev/null; }

# wait_boot PID LOG IP: wait until the guest's lwIP netif reports the static
# address (printed from kernel/net/lwip_stack.c during boot), or the guest
# dies, or the timeout elapses.
wait_boot() {  # pid log expected_ip
    local pid=$1 logfile=$2 ip=$3 waited=0
    while (( waited < BOOT_TIMEOUT )); do
        alive "$pid" || { log_fail_tail "$logfile"; die "guest pid $pid exited during boot"; }
        if grep -q "ip=$ip" "$logfile" 2>/dev/null; then
            log "guest pid $pid booted, netif at $ip (waited ${waited}s)"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    log_fail_tail "$logfile"
    die "guest pid $pid did not report ip=$ip within ${BOOT_TIMEOUT}s (set A20_CLUSTER_BOOT_TIMEOUT to override)"
}

wait_boot "$PID_A" "$LOG_A" "$GUEST_IP_A"
wait_boot "$PID_B" "$LOG_B" "$GUEST_IP_B"
sleep 3   # let the shell reach its prompt before the first command lands

# -------------------------------------------------------------- ping proofs

# ping_once FROM_FD FROM_LOG DST: send `ping DST 3` into the guest console and
# wait for the all-received statistics line the guest's ping prints
# (user/cmds/net/ping.c: "%d packets transmitted, %d received").  A command
# sent before the console was ready can be dropped, so the injection is
# retried; what is never tolerated is a stats line with losses.
ping_once() {  # fifo_fd logfile dst label
    local fd=$1 logfile=$2 dst=$3 label=$4 attempt=1
    local want="3 packets transmitted, 3 received"
    while (( attempt <= 3 )); do
        log "$label: ping $dst 3 (attempt $attempt)"
        printf 'ping %s 3\n' "$dst" >&"$fd"
        local waited=0
        while (( waited < PING_TIMEOUT )); do
            alive "$PID_A" || { log_fail_tail "$LOG_A"; die "$label: guest A exited during the ping proof"; }
            alive "$PID_B" || { log_fail_tail "$LOG_B"; die "$label: guest B exited during the ping proof"; }
            if grep -q "$want" "$logfile" 2>/dev/null; then
                grep "$want" "$logfile" | tail -n 1 | sed "s/^/$label: /"
                return 0
            fi
            if grep -q "packets transmitted, [012] received" "$logfile" 2>/dev/null; then
                grep "packets transmitted" "$logfile" | tail -n 1 | sed "s/^/$label: /" >&2
                log_fail_tail "$logfile"
                die "$label: ping $dst lost packets (see above)"
            fi
            sleep 1
            waited=$((waited + 1))
        done
        attempt=$((attempt + 1))
    done
    log_fail_tail "$logfile"
    die "$label: no '$want' line within $((3 * PING_TIMEOUT))s -- the tunnel or the console injection is broken"
}

ping_once 50 "$LOG_A" "$GUEST_IP_B" "A($GUEST_IP_A) -> B($GUEST_IP_B)"
ping_once 51 "$LOG_B" "$GUEST_IP_A" "B($GUEST_IP_B) -> A($GUEST_IP_A)"

# ------------------------------------------------------------------- verdict

if [[ "$KEEP" != 1 && "$KEEP" != true ]]; then
    log "powering off both guests"
    printf 'poweroff\n' >&50 2>/dev/null || true
    printf 'poweroff\n' >&51 2>/dev/null || true
    local_wait=0
    while (( local_wait < 20 )) && { alive "$PID_A" || alive "$PID_B"; }; do
        sleep 1
        local_wait=$((local_wait + 1))
    done
    kill "$PID_A" "$PID_B" 2>/dev/null || true
fi

cat > "$LOG_DIR/summary.txt" <<EOF
cluster-net-up: PASS
date: $(date -u +%Y-%m-%dT%H:%M:%SZ)
instances: $INST_A ($GUEST_IP_A, MAC 52:54:00:12:34:01, host UDP 127.0.0.1:$PORT_A)
           $INST_B ($GUEST_IP_B, MAC 52:54:00:12:34:02, host UDP 127.0.0.1:$PORT_B)
A -> B ping: 3 packets transmitted, 3 received
B -> A ping: 3 packets transmitted, 3 received
launch parameters: $LOG_DIR/$INST_A.cmdline, $LOG_DIR/$INST_B.cmdline
guest transcripts: $LOG_A, $LOG_B
EOF
cat "$LOG_DIR/summary.txt"
log "PASS -- the UDP tunnel between the two instances is up; next: WA2 cluster transport (docs/cluster/02-udp-demo.md)"
exit 0
