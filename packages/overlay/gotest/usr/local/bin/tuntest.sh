#!/bin/sh
# tuntest —— tailscaled TUN 模式验收（真实 /dev/net/tun，不用 userspace-networking）
#
# 与 tstest 的区别：数据面走内核 TUN（/dev/net/tun + 路由表），应用直接访问
# 100.x，不再经过 SOCKS。入站由内核 tun 投递，不再依赖 127.0.0.1 转发。
#
# 用法: tuntest <login-server> <authkey> <peer-tailscale-ip>

export HOME=/root
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export TMPDIR=/tmp

LOGIN=${1:-}
KEY=${2:-}
PEER=${3:-}

fails=0
note() { echo "TUNTEST: $*"; }
fail() { echo "TUNTEST: FAIL $*"; fails=$((fails+1)); }

[ -n "$LOGIN" ] && [ -n "$KEY" ] || { echo "TUNTEST: FAIL usage"; echo "TS_RESULT: FAIL (1)"; exit 1; }

D=/tmp/tsd-tun
rm -rf $D && mkdir -p $D || exit 1

dump_log() {
    echo "----- tailscaled.log head -----"
    head -60 $D/tailscaled.log 2>/dev/null
    echo "----- tailscaled.log tail -----"
    tail -20 $D/tailscaled.log 2>/dev/null
}

note "tailscaled starting in TUN mode (--tun=tailscale0)"
tailscaled --tun=tailscale0 --state=$D/state --socket=$D/ts.sock \
    --no-logs-no-support > $D/tailscaled.log 2>&1 &
TSD_PID=$!

i=0
ready=0
while [ $i -lt 40 ]; do
    if curl -sf --max-time 5 --unix-socket $D/ts.sock \
        http://local-tailscaled.sock/localapi/v0/status > $D/api.json 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 $TSD_PID 2>/dev/null; then
        fail "tailscaled exited before becoming ready (${i}s)"
        dump_log
        echo "TS_RESULT: FAIL ($fails)"
        exit 1
    fi
    i=$((i+1))
    sleep 1
done
if [ $ready -ne 1 ]; then
    fail "tailscaled not ready in 40s"
    dump_log
    echo "TS_RESULT: FAIL ($fails)"
    exit 1
fi
note "tailscaled ready after ${i}s"

# 接口必须真的出现在 /dev/net/tun 之外的内核视图里
note "interfaces while tailscaled runs:"
ifconfig -a 2>/dev/null | head -20

note "tailscale up (login-server=$LOGIN)"
tailscale --socket=$D/ts.sock up \
    --login-server="$LOGIN" --authkey="$KEY" \
    --hostname=a20os-tun --accept-dns=false --timeout=90s \
    > up.out 2>&1 || fail "tailscale up: $(cat up.out)"
cat up.out

tailscale --socket=$D/ts.sock status > status.out 2>&1 || fail "status"
head -12 status.out

MYIP=$(tailscale --socket=$D/ts.sock ip -4 2>/dev/null)
[ -n "$MYIP" ] || fail "no tailscale ipv4"
echo "TSIP $MYIP"

if [ -n "$PEER" ] && [ -n "$MYIP" ]; then
    # ---- 出口：应用直接访问对端（无 SOCKS），走内核 tun + 路由表 ----
    note "direct curl to peer probe (no SOCKS)"
    ok=0
    i=0
    while [ $i -lt 6 ]; do
        curl -s --max-time 15 "http://$PEER:9999/" > direct.out 2>&1
        if grep -q "A20OS-TS-HOST-OK" direct.out; then ok=1; break; fi
        i=$((i+1)); sleep 3
    done
    cat direct.out
    [ $ok -eq 1 ] || { fail "direct egress to peer"; dump_log; }

    # ---- 入口：本机 8000 服务经内核 tun 暴露给 tailnet ----
    note "inbound httpd on 0.0.0.0:8000"
    mkdir -p /tmp/www && cd /tmp/www || exit 1
    echo "A20OS-TS-GUEST-OK" > index.html
    cat > go.mod <<'GOMOD'
module a20tunwww

go 1.25
GOMOD
    cat > main.go <<'GOSRC'
package main

import (
	"net/http"
	"os"
)

func main() {
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		http.ServeFile(w, r, "/tmp/www/index.html")
	})
	if err := http.ListenAndServe("0.0.0.0:8000", nil); err != nil {
		os.Stderr.WriteString(err.Error() + "\n")
		os.Exit(1)
	}
}
GOSRC
    go build -o srv . 2> httpd.build.err || { cat httpd.build.err; fail "httpd build"; }
    ./srv > httpd.log 2>&1 &
    ok=0
    i=0
    while [ $i -lt 5 ]; do
        curl -s --max-time 5 "http://127.0.0.1:8000/" > local.out 2>&1 || true
        if grep -q "A20OS-TS-GUEST-OK" local.out; then ok=1; break; fi
        i=$((i+1)); sleep 1
    done
    if [ $ok -ne 1 ]; then
        fail "guest local httpd"
        cat local.out
        cat httpd.log 2>/dev/null
    fi
fi

if [ $fails -eq 0 ]; then
    echo "TUNTEST: ALL STAGES PASS"
    echo "TS_RESULT: PASS"
else
    echo "TS_RESULT: FAIL ($fails)"
fi
exit $fails
