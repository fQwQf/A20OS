#!/bin/sh
# mmtest —— 单级内存模型的真实软件门禁（guest 侧）
#
# 与 smoke-mm-stress 的分工：那个是内核自带的合成压力程序，这个是**真实软件**
# 在模型上跑。两者覆盖的失效模式不同——真实软件会做合成程序不会做的事：
#
#   * git    大量小文件 mmap + page cache，VMA 频繁插入/删除/合并
#   * vim    mprotect 在 PROT_READ|PROT_WRITE 之间来回翻转，mmap 共享段反复映射
#   * gcc    fork+exec 风暴 + 链接器写只读段，COW 的主战场
#   * python 解释器 arena 的 brk/mmap 抖动
#   * node   V8 堆增长 + madvise(DONTNEED) 批量回收
#
# 每个阶段都校验**内容**而不只是退出码：只查退出码会漏掉"跑完了但数据算错"
# 这类最阴的 MM 错误（页被回收后仍可读、COW 后父子共享了本该私有的页）。
#
# 最后打印 MMTEST_AUDIT 一行，由宿主 grep；宿主的判定门是这一行全 0，
# 而不是任何一个 PASS 标记。

export HOME=/root
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export TMPDIR=/tmp
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_SYSTEM=/dev/null

W=/tmp/mmtest
mkdir -p $W || exit 1
cd $W || exit 1

fails=0
note() { echo "MMTEST: $*"; }
fail() { echo "MMTEST: FAIL $*"; fails=$((fails+1)); }

# ---- 1. git：大量小文件，page cache + VMA churn -------------------------
note "git start"
rm -rf repo && mkdir repo && cd repo || fail "git mkdir"
git init -q . 2>/dev/null || fail "git init"
i=0
while [ $i -lt 60 ]; do
    f=src$i.c
    {
        echo "#include <stdio.h>"
        echo "int f$i(void){ return $i; }"
        i2=0
        while [ $i2 -lt 20 ]; do
            echo "/* padding $i $i2 padding padding padding padding */"
            i2=$((i2+1))
        done
    } > $f
    git add $f 2>/dev/null || fail "git add $f"
    i=$((i+1))
done
git -c user.email=a@b -c user.name=a commit -qm one 2>/dev/null || fail "git commit"
# 二进制文件：走 mmap 读回，page cache 路径
i=0
while [ $i -lt 30 ]; do
    dd if=/dev/urandom of=blob$i.bin bs=1024 count=8 2>/dev/null
    git add blob$i.bin 2>/dev/null || fail "git add blob"
    i=$((i+1))
done
git -c user.email=a@b -c user.name=a commit -qm two 2>/dev/null || fail "git commit2"
git status --porcelain > $W/git.status 2>/dev/null
[ -s $W/git.status ] && fail "git dirty after commit"
git log --oneline > $W/git.log 2>/dev/null
grep -q "two" $W/git.log || fail "git log missing two"
# clone：大批量 mmap + 写时复制
cd $W || fail "cd"
rm -rf clone && git clone -q repo clone 2>/dev/null || fail "git clone"
cd clone || fail "cd clone"
# 内容必须与源一致（页回收后读回错误内容在这里暴露）
if ! cmp -s src7.c ../repo/src7.c; then fail "git clone content mismatch"; fi
if ! cmp -s blob3.bin ../repo/blob3.bin; then fail "git clone blob mismatch"; fi
cd $W || fail "cd up"
note "git done"

# ---- 2. vim：mprotect 翻转 + 保存 ---------------------------------------
note "vim start"
printf 'line one\nline two\nline three\n' > $W/v.txt
# -es 为 silent ex 模式，避免需要终端
vim -es -u NONE -i NONE -N \
    -c 'set nomore' \
    -c 'normal! gg' \
    -c 'normal! oinserted line' \
    -c 'wq' $W/v.txt </dev/null >/dev/null 2>&1
grep -q "inserted line" $W/v.txt || fail "vim did not write"
printf 'a\nb\nc\nd\ne\nf\ng\n' > $W/v2.txt
vim -es -u NONE -i NONE -N \
    -c '%s/a/A/' -c 'wq' $W/v2.txt </dev/null >/dev/null 2>&1
grep -q "^A$" $W/v2.txt || fail "vim substitute did not persist"
# 大文件：mmap + 反复 mprotect 写回
i=0
: > $W/big.txt
while [ $i -lt 4000 ]; do echo "0123456789012345678901234567890123456789" >> $W/big.txt; i=$((i+1)); done
vim -es -u NONE -i NONE -N -c 'normal! Gdd' -c 'wq' $W/big.txt </dev/null >/dev/null 2>&1
lines=$(wc -l < $W/big.txt 2>/dev/null)
[ "$lines" = "3999" ] || fail "vim big file line count=$lines want 3999"
note "vim done"

# ---- 3. gcc：fork/exec 风暴 + COW + 链接器写只读段 ---------------------
note "gcc start"
mkdir -p gsrc && cd gsrc || fail "gcc mkdir"
cat > main.c <<'EOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static char big[1 << 20];
int main(int argc, char **argv) {
    memset(big, argc, sizeof(big));
    unsigned long s = 0;
    for (size_t i = 0; i < sizeof(big); i += 4096) s += (unsigned char)big[i];
    printf("sum=%lu argc=%d\n", s, argc);
    return 0;
}
EOF
# 多文件 + 头文件，逼近真实编译的映射数量
i=0
while [ $i -lt 12 ]; do
    printf '#define K%d %d\n' $i $i > h$i.h
    printf '#include "h%d.h"\nint g%d(void){return K%d;}\n' $i $i $i > g$i.c
    i=$((i+1))
done
{
    i=0
    while [ $i -lt 12 ]; do printf 'int g%d(void);\n' $i; i=$((i+1)); done
    printf 'int main(int argc,char**argv);\n'
} > all.h
gcc -O2 -I. -o app main.c g0.c g1.c g2.c g3.c g4.c g5.c g6.c g7.c g8.c g9.c g10.c g11.c 2>cc.err || {
    cat cc.err; fail "gcc compile"; }
[ -x ./app ] || fail "gcc produced no binary"
./app > run.out 2>&1 || fail "gcc app run"
grep -q "argc=1" run.out || fail "gcc app output wrong: $(cat run.out)"
# 反复运行：每次 exec 都是新的地址空间 + COW
i=0
while [ $i -lt 15 ]; do
    ./app > /dev/null 2>&1 || fail "gcc app rerun $i"
    i=$((i+1))
done
# 静态链接：更大的映射集
gcc -O2 -static -I. -o apps main.c g*.c 2>cc2.err || { cat cc2.err; fail "gcc static"; }
./apps > /dev/null 2>&1 || fail "gcc static run"
# fork 风暴：父子同时写同一批继承页，COW 必须给出私有副本
cat > forker.c <<'EOF'
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
static char page[1 << 16];
int main(void) {
    memset(page, 0x5a, sizeof(page));
    for (int round = 0; round < 40; round++) {
        pid_t p = fork();
        if (p == 0) {
            for (size_t i = 0; i < sizeof(page); i += 512)
                page[i] = (char)(round + (i & 0x7f));
            _exit(page[0] == (char)round ? 0 : 1);
        }
        if (p < 0) return 2;
        /* 父进程在子进程运行期间继续改同一批页 */
        for (size_t i = 0; i < sizeof(page); i += 512)
            page[i] = (char)(round ^ 0x3f);
        int st = 0;
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return 3;
        for (size_t i = 0; i < sizeof(page); i += 512)
            if (page[i] != (char)(round ^ 0x3f)) return 4;
    }
    printf("fork ok\n");
    return 0;
}
EOF
gcc -O2 -o forker forker.c 2>f.err || { cat f.err; fail "gcc forker"; }
./forker > fork.out 2>&1 || fail "fork storm: $(cat fork.out)"
grep -q "fork ok" fork.out || fail "fork storm output"
cd $W || fail "cd up from gcc"
note "gcc done"

# ---- 4. python：解释器 arena 抖动 ---------------------------------------
note "python start"
python3 -c 'import sys; print(sys.version_info[0])' > py.v 2>&1 || fail "python run"
grep -qE '^(3|2)' py.v || fail "python version: $(cat py.v)"
python3 - <<'EOF' > py.out 2>&1 || fail "python script"
import gc, sys
# 反复分配/释放，逼迫解释器走 brk 与 mmap 两条路
tot = 0
for round in range(300):
    xs = [bytearray(4096) for _ in range(64)]
    for j, b in enumerate(xs):
        b[0] = (round + j) & 0xff
        tot += b[0]
    del xs
    if round % 50 == 0:
        gc.collect()
# 大对象：mmap 路径
big = bytearray(4 << 20)
big[0] = 1
big[-1] = 2
tot += big[0] + big[-1]
print("pytot", tot)
EOF
grep -q "pytot" py.out || fail "python output: $(cat py.out)"
# python 加载自身 .so 与 stdlib：大量 file-backed mmap
python3 -c 'import json,re,collections,ctypes,ssl' > py2.out 2>&1 || fail "python imports"
python3 -c 'import ctypes; print(ctypes.sizeof(ctypes.c_void_p))' > py3.out 2>&1 || fail "python ctypes"
note "python done"

# ---- 5. node：V8 堆增长 + madvise 回收 ----------------------------------
note "node start"
node -e 'console.log("nodever", process.version)' > node.v 2>&1 || fail "node run"
grep -q nodever node.v || fail "node output: $(cat node.v)"
node -e '
const big = [];
for (let r = 0; r < 200; r++) {
  const a = new Array(20000).fill(r);
  for (let i = 0; i < a.length; i += 97) a[i] = (r * i) & 0xffff;
  big.push(a.length);
}
let s = 0; for (const v of big) s += v;
console.log("nodesum", s);
' > node2.out 2>&1 || fail "node alloc"
grep -q nodesum node2.out || fail "node alloc output: $(cat node2.out)"
node -e 'const m=new Map(); for(let i=0;i<50000;i++) m.set("k"+i,{v:i}); console.log("nodemap", m.size);' > node3.out 2>&1 || fail "node map"
grep -q "nodemap 50000" node3.out || fail "node map output"
note "node done"

# ---- 6. 汇总：内核审计行必须全 0 ---------------------------------------
# mm_pt_audit_all() 在 shutdown 路径上跑；这里只把阶段结果汇总。
if [ $fails -eq 0 ]; then
    echo "MMTEST: ALL STAGES PASS"
    echo "MMTEST_RESULT: PASS"
else
    echo "MMTEST_RESULT: FAIL ($fails)"
fi
exit $fails
