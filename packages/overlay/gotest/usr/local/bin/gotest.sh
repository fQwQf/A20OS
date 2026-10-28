#!/bin/sh
# gotest —— Go 工具链兼容性门禁（guest 侧，x86_64 KVM / riscv64 TCG 皆可）
#
# 与 mmtest（git/vim/gcc/python/nodejs）的分工：mmtest 证明 MM/VFS/调度在
# "传统"用户态负载下正确，本脚本针对 Go 运行时的特殊路径：
#
#   * goroutine 调度建立在 raw clone(CLONE_THREAD) + futex 唤醒之上，
#     与 libc pthread 的使用方式不同（自管线程栈、自旋后退）
#   * runtime 依赖 getrandom、/proc/self/maps、内存归还 (madvise DONTNEED)
#   * net.Interfaces() 走 AF_NETLINK NETLINK_ROUTE (RTM_GETLINK/RTM_GETADDR)
#   * loopback HTTP 走 socket 层 listener（fast 模式下 listener 不进 lwIP）
#   * 出站 TLS 验证 ca-certificates + DNS + conntrack UDP 路径
#
# 每个阶段校验内容而不是只看退出码。结果行 GOTEST_RESULT 由宿主 grep；
# cgo 单独给 CGO_PROBE 行（tailscale 的 apk 构建是 CGO_ENABLED=0，
# cgo 失败不否决门禁，但必须如实记录）。

export HOME=/root
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export TMPDIR=/tmp
export GOTOOLCHAIN=local
export GOPROXY=off
export GOFLAGS=
export CGO_ENABLED=1

W=/tmp/gotest
rm -rf $W && mkdir -p $W || exit 1
cd $W || exit 1

fails=0
note() { echo "GOTEST: $*"; }
fail() { echo "GOTEST: FAIL $*"; fails=$((fails+1)); }

# ---- 0. go 自身可执行、环境完整 -----------------------------------------
note "stage0 start (go version/env)"
go version > go.version 2>&1 || fail "go version"
cat go.version
grep -q "go version go1" go.version || fail "go version output"
go env GOOS GOARCH GOROOT GOCACHE CGO_ENABLED > go.env 2>&1 || fail "go env"
cat go.env
grep -q "linux" go.env || fail "GOOS not linux"
note "stage0 done"

# ---- 1. goroutine/chan/sync/atomic：调度器 + futex + clone 线程 ----------
note "stage1 start (goroutine scheduler)"
mkdir -p sched && cd sched || exit 1
cat > go.mod <<'EOF'
module a20sched

go 1.25
EOF
cat > main.go <<'EOF'
package main

import (
	"fmt"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
)

func main() {
	workers := runtime.GOMAXPROCS(0) * 8
	ch := make(chan int64, workers)
	var wg sync.WaitGroup
	var sum int64
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			var s int64
			for i := 0; i < 200000; i++ {
				s += int64((i * id) ^ (i << 3))
				if i%4096 == 0 {
					runtime.Gosched()
				}
			}
			atomic.AddInt64(&sum, s)
			ch <- s
		}(w)
	}
	go func() { wg.Wait(); close(ch) }()
	var drained int64
	for v := range ch {
		drained += v
	}
	if drained != atomic.LoadInt64(&sum) {
		fmt.Fprintln(os.Stderr, "checksum mismatch")
		os.Exit(1)
	}
	if runtime.NumGoroutine() != 1 {
		fmt.Fprintln(os.Stderr, "goroutine leak:", runtime.NumGoroutine())
		os.Exit(1)
	}
	fmt.Printf("GOSCHED OK workers=%d sum=%d\n", workers, drained)
}
EOF
go build -o schedprobe . 2> build.err || { cat build.err; fail "go build sched"; }
[ -x ./schedprobe ] || fail "no schedprobe binary"
./schedprobe > run.out 2>&1 || fail "schedprobe run: $(cat run.out)"
grep -q "GOSCHED OK" run.out || fail "schedprobe output: $(cat run.out)"
# 反复 exec：每次都是新的地址空间 + 全新线程组
i=0
while [ $i -lt 5 ]; do
    ./schedprobe > /dev/null 2>&1 || fail "schedprobe rerun $i"
    i=$((i+1))
done
cd $W || exit 1
note "stage1 done"

# ---- 2. loopback HTTP + netlink：server/client 同进程 --------------------
note "stage2 start (loopback http + netlink)"
mkdir -p netw && cd netw || exit 1
cat > go.mod <<'EOF'
module a20net

go 1.25
EOF
cat > main.go <<'EOF'
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"time"
)

func main() {
	ifs, err := net.Interfaces()
	if err != nil {
		fmt.Println("INTERFACES ERR", err)
		os.Exit(1)
	}
	for _, f := range ifs {
		fmt.Printf("IFACE %s flags=%s mtu=%d\n", f.Name, f.Flags.String(), f.MTU)
	}
	body := "A20OS-LOOPBACK-OK-7f001"
	done := make(chan error, 1)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Println("LISTEN ERR", err)
		os.Exit(1)
	}
	addr := ln.Addr().String()
	mux := http.NewServeMux()
	mux.HandleFunc("/probe", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, body)
	})
	srv := &http.Server{Handler: mux}
	go func() { done <- srv.Serve(ln) }()
	client := &http.Client{Timeout: 15 * time.Second}
	resp, err := client.Get("http://" + addr + "/probe")
	if err != nil {
		fmt.Println("HTTP GET ERR", err)
		os.Exit(1)
	}
	b, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	srv.Close()
	got := strings.TrimSpace(string(b))
	fmt.Printf("HTTP status=%d body=%s addr=%s\n", resp.StatusCode, got, addr)
	if resp.StatusCode != 200 || got != body {
		os.Exit(1)
	}
	if err := <-done; err != nil && err != http.ErrServerClosed {
		fmt.Println("SERVE ERR", err)
		os.Exit(1)
	}
	fmt.Println("LOOPBACK OK")
}
EOF
go build -o netprobe . 2> build.err || { cat build.err; fail "go build net"; }
./netprobe > run.out 2>&1 || fail "netprobe run: $(cat run.out)"
grep -q "LOOPBACK OK" run.out || fail "netprobe output: $(cat run.out)"
grep -q "IFACE lo" run.out || fail "netlink interfaces: $(cat run.out)"
cd $W || exit 1
note "stage2 done"

# ---- 3. go test：testing 框架 + t.Parallel + 子测试 ----------------------
note "stage3 start (go test)"
mkdir -p pkg && cd pkg || exit 1
cat > go.mod <<'EOF'
module a20pkg

go 1.25
EOF
cat > pkg.go <<'EOF'
package a20pkg

import "strings"

func Repeat(s string, n int) string { return strings.Repeat(s, n) }
EOF
cat > pkg_test.go <<'EOF'
package a20pkg

import (
	"strings"
	"sync"
	"testing"
)

func TestParallelGoroutines(t *testing.T) {
	var mu sync.Mutex
	// Indexed by the goroutine's own n, not by completion order: appending
	// and then asserting results[i] belongs to i is a test bug (the 99-repeat
	// goroutine can finish first), not a scheduler defect.
	results := make([]string, 100)
	var wg sync.WaitGroup
	for i := 0; i < 100; i++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			s := Repeat("ab", n)
			mu.Lock()
			results[n] = s
			mu.Unlock()
		}(i)
	}
	wg.Wait()
	for i, s := range results {
		if strings.Count(s, "ab") != i {
			t.Fatalf("result %d wrong: %d repeats", i, strings.Count(s, "ab"))
		}
	}
}

func TestSub(t *testing.T) {
	t.Parallel()
	t.Run("inner", func(t *testing.T) {
		t.Parallel()
		if Repeat("x", 3) != "xxx" {
			t.Fatal("repeat broken")
		}
	})
}
EOF
go test -count=1 ./... > test.out 2>&1 || { cat test.out; fail "go test"; }
grep -q "^ok[[:space:]]" test.out || fail "go test output: $(cat test.out)"
cd $W || exit 1
note "stage3 done"

# ---- 4. cgo：外部链接 + C 互操作（记录项，不否决门禁） -------------------
note "stage4 start (cgo probe)"
mkdir -p cgo && cd cgo || exit 1
cat > go.mod <<'EOF'
module a20cgo

go 1.25
EOF
cat > main.go <<'EOF'
package main

/*
#include <stdlib.h>
#include <string.h>
static char *mirror(const char *src, int n) {
	char *dst = (char *)malloc(n + 1);
	memcpy(dst, src, n);
	dst[n] = 0;
	return dst;
}
*/
import "C"
import (
	"fmt"
	"unsafe"
)

func main() {
	src := "cgo-probe-A20OS"
	cs := C.CString(src)
	defer C.free(unsafe.Pointer(cs))
	gs := C.GoString(C.mirror(cs, C.int(len(src))))
	fmt.Println("CGO ECHO", gs)
}
EOF
if go build -o cgoprobe . 2> build.err; then
    ./cgoprobe > run.out 2>&1 || fail "cgoprobe run: $(cat run.out)"
    if grep -q "CGO ECHO cgo-probe-A20OS" run.out; then
        echo "CGO_PROBE: PASS"
    else
        echo "CGO_PROBE: FAIL output: $(cat run.out)"
    fi
else
    echo "CGO_PROBE: FAIL build:"; cat build.err
fi
cd $W || exit 1
note "stage4 done"

# ---- 5. 出站 HTTPS：ca 证书 + DNS + TLS + conntrack UDP ------------------
note "stage5 start (outbound https)"
mkdir -p tls && cd tls || exit 1
cat > go.mod <<'EOF'
module a20tls

go 1.25
EOF
cat > main.go <<'EOF'
package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

func main() {
	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Get("https://dl-cdn.alpinelinux.org/alpine/")
	if err != nil {
		fmt.Println("TLS GET ERR", err)
		os.Exit(1)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	body := strings.ToLower(string(b))
	fmt.Printf("TLS status=%d bytes=%d alpine=%v\n", resp.StatusCode, len(b),
		strings.Contains(body, "alpine"))
	if resp.StatusCode != 200 || !strings.Contains(body, "alpine") {
		os.Exit(1)
	}
	fmt.Println("OUTBOUND TLS OK")
}
EOF
go build -o tlsprobe . 2> build.err || { cat build.err; fail "go build tls"; }
./tlsprobe > run.out 2>&1 || fail "tlsprobe run: $(cat run.out)"
grep -q "OUTBOUND TLS OK" run.out || fail "tlsprobe output: $(cat run.out)"
cd $W || exit 1
note "stage5 done"

# ---- 6. 汇总 -------------------------------------------------------------
if [ $fails -eq 0 ]; then
    echo "GOTEST: ALL STAGES PASS"
    echo "GOTEST_RESULT: PASS"
else
    echo "GOTEST_RESULT: FAIL ($fails)"
fi
exit $fails
