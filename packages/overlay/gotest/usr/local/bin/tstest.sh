#!/bin/sh
# tstest —— tailscale 用户态组网评估（guest 侧）
#
# 内核没有 TUN/TAP 驱动（kernel/ 全树无 TUNSETIFF），所以 tailscaled 必须
# 跑 --tun=userspace-networking：WireGuard 与 DERP 终结在 tailscaled 自己的
# netstack，本地应用经 127.0.0.1 SOCKS5 出站；对端进站的流量由 netstack
# 转发到 127.0.0.1 同端口的服务。
#
# 用法: tstest <login-server> <authkey> <peer-tailscale-ip>
#   <login-server>   headscale 控制面 URL（宿主侧 docker，经 slirp 可达）
#   <authkey>        headscale 预授权密钥
#   <peer-tailscale-ip>  宿主侧第二个节点（tailscaled userspace）的 100.64 地址
#
# 出站方向: curl --socks5-hostname 127.0.0.1:1055 http://<peer>:9999/
#           -> guest tailscaled SOCKS5 -> wireguard/DERP -> 宿主 netstack
#           -> 宿主 127.0.0.1:9999 的探针服务
# 入站方向: 宿主 curl --socks5-hostname 127.0.0.1:1056 http://<本机>:8000/
#           由宿主驱动脚本执行；本脚本只负责把 busybox httpd 留在 8000 端口。
# 结果行 TS_RESULT 由宿主 grep。

LOGIN=${1:-}
KEY=${2:-}
PEER=${3:-}

export HOME=/root
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export TMPDIR=/tmp

fails=0
note() { echo "TSTEST: $*"; }
fail() { echo "TSTEST: FAIL $*"; fails=$((fails+1)); }

[ -n "$LOGIN" ] && [ -n "$KEY" ] || { echo "TSTEST: FAIL usage"; echo "TS_RESULT: FAIL (1)"; exit 1; }

D=/tmp/tsd
rm -rf $D && mkdir -p $D || exit 1

# ---- 0. 二进制可执行 -----------------------------------------------------
tailscale version > ts.version 2>&1 || fail "tailscale version"
cat ts.version
grep -q "1\." ts.version || fail "tailscale version output"

# ---- 0b. 网络现状（eth0/DHCP 由内核 lwIP 完成；这里只记录） -------------
note "network state before tailscaled:"
ifconfig -a 2>/dev/null | head -24
note "resolv.conf: $(head -3 /etc/resolv.conf 2>/dev/null | tr '\n' ' ')"

# ---- 1. tailscaled（userspace netstack + SOCKS5） ------------------------
# 两个日志视图：启动失败的原因在日志开头，shutdown/panic 现场在结尾。
dump_tsd_log() {
    echo "----- tailscaled.log head (startup) -----"
    head -80 $D/tailscaled.log
    echo "----- tailscaled.log tail (shutdown) -----"
    tail -12 $D/tailscaled.log
}

note "tailscaled starting (userspace-networking, socks5 127.0.0.1:1055)"
# --no-logs-no-support: logtail 会去拨 log.tailscale.com（本环境不可达），
# 其关闭竞争在离线环境里以 "panic: cannot Subscribe on a closed client"
# 收场，掩盖真正的退出原因；headscale 场景也不需要上传日志。
tailscaled --tun=userspace-networking \
    --state=$D/state --socket=$D/ts.sock \
    --socks5-server=127.0.0.1:1055 \
    --no-logs-no-support \
    > $D/tailscaled.log 2>&1 &
TSD_PID=$!

# 隔离实验：先不给它任何客户端，看它自己能不能活过 3 秒。
sleep 3
if ! kill -0 $TSD_PID 2>/dev/null; then
    fail "tailscaled died within 3s with no client attached"
    dump_tsd_log 2>/dev/null || { head -80 $D/tailscaled.log; tail -12 $D/tailscaled.log; }
    echo "TS_RESULT: FAIL ($fails)"
    exit 1
fi
note "tailscaled alive after 3s idle"

# localapi 直连探针：绕开 tailscale CLI，验证 unix socket + HTTP 路径。
note "localapi probe (curl over $D/ts.sock):"
curl -s --max-time 5 --unix-socket $D/ts.sock \
    http://local-tailscaled.sock/localapi/v0/status 2>&1 | head -c 300
echo
if ! kill -0 $TSD_PID 2>/dev/null; then
    fail "tailscaled died during curl localapi probe"
    dump_tsd_log
    echo "TS_RESULT: FAIL ($fails)"
    exit 1
fi
note "tailscaled survived localapi probe"

i=0
ready=0
while [ $i -lt 30 ]; do
    # 就绪以 localapi 能应答为准，不以 `tailscale status` 的退出码为准：
    # daemon 处于 NeedsLogin 时 CLI 会以非零状态退出，但那说明它工作正常。
    if curl -sf --max-time 5 --unix-socket $D/ts.sock \
        http://local-tailscaled.sock/localapi/v0/status > $D/api.json 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 $TSD_PID 2>/dev/null; then
        fail "tailscaled exited before becoming ready (${i}s)"
        dump_tsd_log
        echo "TS_RESULT: FAIL ($fails)"
        exit 1
    fi
    i=$((i+1))
    case $i in
        15|30|45) note "still waiting for tailscaled (${i}s); log tail:"
                  tail -5 $D/tailscaled.log ;;
    esac
    sleep 1
done
if [ $ready -ne 1 ]; then
    fail "tailscaled not ready in 60s"
    note "last tailscale status output:"
    head -12 $D/status.out
    dump_tsd_log
    echo "TS_RESULT: FAIL ($fails)"
    exit 1
fi
note "tailscaled ready after ${i}s (localapi answered)"
note "tailscale status (informational; exit code ignored):"
timeout 10 tailscale --socket=$D/ts.sock status > $D/status.out 2>&1
echo "exit=$? output:"
head -12 $D/status.out

# ---- 2. 接入控制面 -------------------------------------------------------
note "tailscale up (login-server=$LOGIN)"
tailscale --socket=$D/ts.sock up \
    --login-server="$LOGIN" --authkey="$KEY" \
    --hostname=a20os-guest --accept-dns=false --timeout=90s \
    > up.out 2>&1 || fail "tailscale up: $(cat up.out)"
cat up.out

tailscale --socket=$D/ts.sock status > status.out 2>&1 || fail "status"
cat status.out
MYIP=$(tailscale --socket=$D/ts.sock ip -4 2>/dev/null)
[ -n "$MYIP" ] || fail "no tailscale ipv4"
echo "TSIP $MYIP"

# ---- 3. 数据面：tailscale ping（disco/wireguard，含 DERP 中继） ----------
if [ -n "$PEER" ]; then
    note "tailscale ping $PEER"
    ok=0
    i=0
    while [ $i -lt 3 ]; do
        tailscale --socket=$D/ts.sock ping --timeout=10s --c=6 "$PEER" > ping.out 2>&1
        if grep -q "pong" ping.out; then ok=1; break; fi
        i=$((i+1))
        sleep 3
    done
    cat ping.out
    [ $ok -eq 1 ] || fail "tailscale ping $PEER"

    # ---- 4. 出站应用流量：SOCKS5 -> 对端探针服务 -------------------------
    note "curl via socks5 to peer probe"
    ok=0
    i=0
    while [ $i -lt 4 ]; do
        curl -s --max-time 20 --socks5-hostname 127.0.0.1:1055 \
            "http://$PEER:9999/" > proxied.out 2>&1
        if grep -q "A20OS-TS-HOST-OK" proxied.out; then ok=1; break; fi
        i=$((i+1)); sleep 3
    done
    cat proxied.out
    if [ $ok -ne 1 ]; then
        fail "socks5 egress to peer"
        note "tailscaled log tail after socks failure:"
        tail -25 $D/tailscaled.log
    fi
else
    note "no peer given; skipping data-plane stages"
fi

# ---- 5. 入站准备：Go httpd 留在 8000，供宿主经 SOCKS5 回打 -------------
# busybox httpd 由 select() 驱动，在这个内核上没起来；Go 的 net/http 在
# stage2 已证明可用于 127.0.0.1，换它。
mkdir -p /tmp/www && cd /tmp/www || exit 1
echo "A20OS-TS-GUEST-OK" > index.html
cat > go.mod <<'GOMOD'
module a20www

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
	if err := http.ListenAndServe("127.0.0.1:8000", nil); err != nil {
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

# ---- 6. 汇总（进程保持存活，宿主随后做入站回打与关机） -------------------
if [ $fails -eq 0 ]; then
    echo "TSTEST: ALL STAGES PASS"
    echo "TS_RESULT: PASS"
else
    echo "TS_RESULT: FAIL ($fails)"
fi
exit $fails
