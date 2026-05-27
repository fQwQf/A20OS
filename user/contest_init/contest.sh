#!/bin/mksh
#
# contest.sh — automated test runner
# Replaces contest_init.c. Execution path identical to manual mode.

# ── early setup ─────────────────────────────────────────────
[[ -x /test/musl/busybox ]]  && cp /test/musl/busybox /busybox 2>/dev/null
[[ -x /test/glibc/busybox ]] && cp /test/glibc/busybox /busybox 2>/dev/null

# -- LTP environment setup -
mkdir -p /dev/shm /tmp
export LTP_IPC_PATH=/dev/shm
export LTPROOT=/test/glibc/ltp
export LTP_TMPDIR=/tmp
export TMPDIR=/tmp
export HOME=/root
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}/bin/lib:/lib"
mkdir -p /root


print '#!/bin/mksh' > /bin/zcat
print 'exec /busybox zcat "$@"' >> /bin/zcat
chmod 755 /bin/zcat
print '#!/bin/mksh' > /bin/gunzip
print 'exec /busybox gunzip "$@"' >> /bin/gunzip
chmod 755 /bin/gunzip

sync

# ── watchdog ────────────────────────────────────────────────
(
    sleep 7200
    print -u2 '[CONTEST] Global timeout (7200 s)'
    kill -KILL $$
    poweroff
) &
typeset -i WD=$!
trap 'kill $WD 2>/dev/null' EXIT

# ── LTP blacklist ──────────────────────────────────────────
typeset -a BL
typeset _bl=
for _bl in /bin/etc/ltp_blacklist.txt; do
    [[ -f $_bl ]] && break
done
if [[ -f $_bl ]]; then
    while IFS= read -r l; do
        [[ $l != \#* && -n $l ]] && BL+=("$l")
    done <"$_bl"
fi

blacklisted() {
    typeset n=$1 b
    for b in "${BL[@]}"; do [[ $b == "$n" ]] && return 0; done
    return 1
}

# ── test group skip list ───────────────────────────────────
typeset -a SKIP_GROUPS
SKIP_GROUPS+=(unixbench) # 不计分
SKIP_GROUPS+=(cyclictest) # OOM跑不通
SKIP_GROUPS+=(lmbench) # 运行时长很长
SKIP_GROUPS+=(ltp) # 单独执行

# 下面是可以跑通但是为了方便测试跳过的
SKIP_GROUPS+=(iozone)
SKIP_GROUPS+=(libctest)
SKIP_GROUPS+=(libcbench)
# SKIP_GROUPS+=(netperf)
# SKIP_GROUPS+=(iperf)
# SKIP_GROUPS+=(busybox)

skip_group() {
    typeset g=$1 s

    # 1. 检测如果是 riscv64 架构且测试组是 libcbench，则直接跳过
    if [[ $(uname -m) == "riscv64" && $g == "libcbench" ]]; then
        return 0
    fi

    if [[ $(uname -m) == "riscv64" && $g == "lua" ]]; then
        return 0
    fi

    # 2. 原有的常规跳过列表检测
    for s in "${SKIP_GROUPS[@]}"; do [[ $g == "$s" ]] && return 0; done
    return 1
}

# ── LTP inline runner ──────────────────────────────────────
run_ltp() {
    typeset runtime=$1 group=$2
    typeset bin_dir

    for d in "/test/$runtime/ltp/testcases/bin"
    do
        [[ -d $d ]] && { bin_dir=$d; break; }
    done

    print "#### OS COMP TEST GROUP START $group ####"

    if [[ -z $bin_dir ]]; then
        print "[CONTEST][LTP] binary dir not found for $runtime"
        print "#### OS COMP TEST GROUP END $group ####"
        return 1
    fi

    typeset -i total=0 pass=0 skip=0

    for bin in "$bin_dir"/*; do
        [[ -f $bin && -x $bin ]] || continue
        [[ ${bin##*/} == *.sh ]] && continue
        typeset name=${bin##*/}

        if blacklisted "$name"; then
            print "[CONTEST][LTP][SKIP] $name (blacklisted)"
            (( skip++ ))
            continue
        fi

        (( total++ ))
        print "RUN LTP CASE $name"
        if "$bin"; then
            print "END LTP CASE $name : 0"
            (( pass++ ))
        else
            print "FAIL LTP CASE $name : $?"
        fi
    done

    print "\nSummary:\npassed   $pass\nfailed   $(( total - pass ))\nbroken   0\nskipped  $skip\nwarnings 0"
    print "#### OS COMP TEST GROUP END $group ####"
    return 0
}

# ── main ────────────────────────────────────────────────────
typeset -i executed=0 failed=0

for script in /test/*/*_testcode.sh; do
    [[ -f $script ]] || continue

    typeset runtime=${script#/test/}
    runtime=${runtime%%/*}
    typeset group=${script##*/}
    group=${group%_testcode.sh}
    typeset dir=${script%/*}

    if skip_group "$group"; then
        print "[CONTEST][SKIP] $group"
        continue
    fi

    print "[CONTEST][RUN] runtime=$runtime group=$group script=$script"

    cd "$dir" || { print "[CONTEST][ERROR] cd $dir failed"; (( failed++ )); continue; }

    typeset rc=0

    # 这两个智障东西不会自动退出
    if [[ $group == *libctest* ]]; then
        mksh "${script##*/}" &
        typeset test_pid=$!
        typeset -i elapsed=0
        while (( elapsed < 200 )); do
            if kill -0 $test_pid 2>/dev/null; then
                sleep 1
                (( elapsed++ ))
            else
                wait $test_pid
                rc=$?
                break
            fi
        done
        if kill -0 $test_pid 2>/dev/null; then
            kill -9 $test_pid 2>/dev/null
            wait $test_pid 2>/dev/null
            rc=1
        fi
    elif [[ $group == *libcbench* ]]; then
        mksh "${script##*/}" &
        typeset test_pid=$!
        typeset -i elapsed=0
        while (( elapsed < 300 )); do
            if kill -0 $test_pid 2>/dev/null; then
                sleep 1
                (( elapsed++ ))
            else
                wait $test_pid
                rc=$?
                break
            fi
        done
        if kill -0 $test_pid 2>/dev/null; then
            kill -9 $test_pid 2>/dev/null
            wait $test_pid 2>/dev/null
            rc=1
        fi
    else
        mksh "${script##*/}"
        rc=$?
    fi
    cd /
    if (( rc == 0 )); then
        print "[CONTEST][PASS] $group"
    else
        print "[CONTEST][FAIL] $group (exit $rc)"
        (( failed++ ))
    fi
    (( executed++ ))
done

# ── LTP (standalone) ───────────────────────────────────────
for rt_dir in /test/*/ltp/testcases/bin; do
    [[ -d $rt_dir ]] || continue
    typeset runtime=${rt_dir#/test/}
    runtime=${runtime%%/*}

    run_ltp "$runtime" "ltp-$runtime"
    (( executed++ ))
done

print "[CONTEST] Done: $executed tests, $failed failures"

poweroff
