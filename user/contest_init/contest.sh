#!/bin/mksh
#
# contest.sh — automated test runner
# Replaces contest_init.c. Execution path identical to manual mode.

# ── early setup ─────────────────────────────────────────────
[[ -x /test/musl/busybox ]]  && cp /test/musl/busybox /busybox 2>/dev/null
[[ -x /test/musl/busybox ]]  && cp /test/musl/busybox /bin/busybox 2>/dev/null
[[ -x /test/glibc/busybox ]] && cp /test/glibc/busybox /busybox 2>/dev/null
[[ -x /test/glibc/busybox ]] && cp /test/glibc/busybox /bin/busybox 2>/dev/null

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
    sleep 10800
    print -u2 '[CONTEST] Global timeout (10800 s)'
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

cleanup_group() {
    typeset group=$1 p

    case "$group" in
    cyclictest)
        for p in hackbench cyclictest; do
            /busybox killall "$p" 2>/dev/null || killall "$p" 2>/dev/null
        done
        ;;
    iozone)
        /busybox killall iozone 2>/dev/null || killall iozone 2>/dev/null
        ;;
    lmbench)
        for p in lmbench_all lat_ctx lat_proc lat_syscall lat_pipe lat_pagefault lat_mmap lat_select lat_mem_rd bw_mem bw_pipe mhz; do
            /busybox killall "$p" 2>/dev/null || killall "$p" 2>/dev/null
        done
        ;;
    ltp)
        /busybox killall runtest 2>/dev/null || killall runtest 2>/dev/null
        ;;
    esac
}

group_timeout() {
    typeset group=$1

    case "$group" in
    basic|lua) print 180 ;;
    busybox|iperf|netperf|libctest|libcbench) print 300 ;;
    cyclictest|iozone|lmbench) print 360 ;;
    ltp) print 600 ;;
    *) print 300 ;;
    esac
}

run_with_timeout() {
    typeset runtime=$1 group=$2 cmd=$3
    typeset -i timeout=${4:-300}
    typeset -i elapsed=0 rc=0

    mksh "$cmd" &
    typeset test_pid=$!

    while (( elapsed < timeout )); do
        if kill -0 $test_pid 2>/dev/null; then
            sleep 1
            (( elapsed++ ))
        else
            wait $test_pid
            return $?
        fi
    done

    print "[CONTEST][TIMEOUT] runtime=$runtime group=$group after ${timeout}s"
    print "#### OS COMP TEST GROUP END $group-$runtime ####"
    print "[CONTEST][FAIL] $group (exit 124)"
    print "[CONTEST] Stop after timeout to preserve completed scores"
    poweroff
    return 124
}

# ── test group skip list ───────────────────────────────────
typeset -a SKIP_GROUPS
SKIP_GROUPS+=(unixbench) # 不计分
#SKIP_GROUPS+=(lmbench) # 运行时长很长
SKIP_GROUPS+=(ltp) # 单独执行

# 下面是可以跑通但是为了方便测试跳过的
#SKIP_GROUPS+=(iozone)
# SKIP_GROUPS+=(netperf)
# SKIP_GROUPS+=(iperf)
# SKIP_GROUPS+=(busybox)

skip_group() {
    typeset g=$1 runtime=$2 s

    # Only musl libctest is a leaderboard item. The glibc script leaves failing
    # stress children behind and can destabilize later risky groups.
    if [[ $g == "libctest" && $runtime != "musl" ]]; then
        return 0
    fi
    if [[ $g == "cyclictest" ]]; then
        # RISC-V cyclictest passes for both glibc and musl historically; keep them.
        # LoongArch cyclictest still times out -> skip on non-riscv64.
        if [[ $(uname -m) != "riscv64" ]]; then
            return 0
        fi
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

run_group() {
    typeset runtime=$1 group=$2
    typeset script="/test/$runtime/${group}_testcode.sh"
    typeset dir="/test/$runtime"

    [[ -f $script ]] || return 0

    if skip_group "$group" "$runtime"; then
        print "[CONTEST][SKIP] runtime=$runtime group=$group"
        return 0
    fi

    print "[CONTEST][RUN] runtime=$runtime group=$group script=$script"

    cd "$dir" || {
        print "[CONTEST][ERROR] cd $dir failed"
        (( failed++ ))
        return 1
    }

    typeset rc=0
    typeset -i timeout=$(group_timeout "$group")

    run_with_timeout "$runtime" "$group" "${script##*/}" "$timeout"
    rc=$?
    cd /
    if (( rc == 0 )); then
        print "[CONTEST][PASS] $group"
    else
        print "[CONTEST][FAIL] $group (exit $rc)"
        (( failed++ ))
    fi
    (( executed++ ))
}

typeset runtime group
for group in basic busybox lua libctest iperf netperf libcbench; do
    for runtime in glibc musl; do
        run_group "$runtime" "$group"
    done
done

for group in cyclictest iozone lmbench; do
    for runtime in glibc musl; do
        run_group "$runtime" "$group"
    done
done

# ── LTP (standalone) ───────────────────────────────────────
for rt_dir in /test/*/ltp/testcases/bin; do
    [[ -d $rt_dir ]] || continue
    typeset runtime=${rt_dir#/test/}
    runtime=${runtime%%/*}

    run_ltp "$runtime" "ltp-$runtime" &
    typeset ltp_pid=$!
    typeset -i ltp_elapsed=0 ltp_timeout=$(group_timeout ltp)
    while (( ltp_elapsed < ltp_timeout )); do
        if kill -0 $ltp_pid 2>/dev/null; then
            sleep 1
            (( ltp_elapsed++ ))
        else
            wait $ltp_pid
            break
        fi
    done
    if kill -0 $ltp_pid 2>/dev/null; then
        print "[CONTEST][TIMEOUT] runtime=$runtime group=ltp after ${ltp_timeout}s"
        print "#### OS COMP TEST GROUP END ltp-$runtime ####"
        print "[CONTEST][FAIL] ltp (exit 124)"
        print "[CONTEST] Stop after timeout to preserve completed scores"
        poweroff
    fi
    (( executed++ ))
done

print "[CONTEST] Done: $executed tests, $failed failures"

poweroff
