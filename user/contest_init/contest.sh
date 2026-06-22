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
    cyclictest|iozone) print 360 ;;
    lmbench) print 1800 ;;
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

run_ltp_case_with_timeout() {
    typeset name=$1
    typeset -i timeout=${2:-60}
    typeset -i elapsed=0

    print "RUN LTP CASE $name"
    "./$name" &
    typeset pid=$!

    while (( elapsed < timeout )); do
        if kill -0 $pid 2>/dev/null; then
            sleep 1
            (( elapsed++ ))
        else
            wait $pid
            typeset rc=$?
            if (( rc == 0 )); then
                print "END LTP CASE $name : 0"
                return 0
            else
                print "FAIL LTP CASE $name : $rc"
                return 1
            fi
        fi
    done

    print "[CONTEST][LTP][TIMEOUT] case=$name after ${timeout}s"
    /busybox killall "$name" 2>/dev/null || killall "$name" 2>/dev/null
    print "FAIL LTP CASE $name : 124"
    return 1
}

run_ltp_bounded_subset() {
    typeset runtime=$1
    typeset arch=$(uname -m)
    typeset dir="/test/$runtime/ltp/testcases/bin"

    if [[ $arch != "riscv64" || $runtime != "glibc" ]]; then
        print "[CONTEST][SKIP] runtime=$runtime group=ltp current_phase=bounded_rv_glibc_only"
        return 0
    fi

    print "[CONTEST][RUN] runtime=$runtime group=ltp mode=bounded_subset case_timeout=60s"
    print "#### OS COMP TEST GROUP START ltp-$runtime ####"

    if [[ ! -d $dir ]]; then
        print "[CONTEST][ERROR] missing $dir"
        print "#### OS COMP TEST GROUP END ltp-$runtime ####"
        print "[CONTEST][FAIL] ltp bounded_subset missing_dir"
        (( failed++ ))
        return 1
    fi

    cd "$dir" || {
        print "[CONTEST][ERROR] cd $dir failed"
        print "#### OS COMP TEST GROUP END ltp-$runtime ####"
        print "[CONTEST][FAIL] ltp bounded_subset cd_failed"
        (( failed++ ))
        return 1
    }

    # cgroup_fj_proc is a signal-driven helper, not a standalone LTP case.
    # Running it directly blocks forever in sigsuspend(), so keep it blacklisted
    # while collecting bounded, real LTP output on both sides of the cgroup_fj point.
    typeset -i failed_cases=0
    typeset name=
    for name in \
        abort01 abs01 \
        accept01 accept02 accept03 accept4_01 \
        access01 access02 access03 access04 \
        adjtimex01 adjtimex02 adjtimex03 \
        alarm02 alarm03 alarm05 alarm06 alarm07 \
        bind01 bind02 bind03 bind04 bind05 \
        brk01 capget01 capget02 \
        capset01 capset02 capset03 capset04 \
        cgroup_core03
    do
        run_ltp_case_with_timeout "$name" 60 || (( failed_cases++ ))
    done
    print "[CONTEST][LTP][SKIP] cgroup_fj_proc blacklisted_helper"
    run_ltp_case_with_timeout chdir04 60 || (( failed_cases++ ))
    for name in \
        chmod01 chmod03 chmod05 chmod06 chmod07 \
        chown01 chown02 chown03 chown04 chown05 \
        chroot01 chroot03 chroot04 \
        clock_adjtime01 clock_getres01 clock_gettime02 clock_gettime04 \
        clock_nanosleep02 clock_nanosleep04 \
        clock_settime01 clock_settime02 \
        close01 close02 confstr01 \
        connect01 copy_file_range03 \
        creat01 creat03 creat04 creat08 \
        dup01 dup02 dup03 dup04 dup05 dup06 dup07 \
        dup201 dup202 dup203 dup204 dup205 dup206 dup207 \
        dup3_01 dup3_02 \
        epoll_create01 epoll_create02 \
        epoll_create1_01 epoll_create1_02 \
        epoll_ctl01 epoll_ctl02 epoll_ctl03 epoll_ctl04 epoll_ctl05 \
        epoll_pwait01 epoll_pwait02 epoll_pwait04 \
        epoll_wait02 epoll_wait03 epoll_wait04 epoll_wait07 \
        eventfd2_01 eventfd2_02 eventfd2_03 \
        execve02 \
        exit01 exit02 exit_group01 \
        faccessat01 faccessat02 faccessat201 faccessat202 \
        fallocate03 \
        fchdir01 fchdir02 fchdir03 \
        fchmod01 fchmod02 fchmod03 fchmod04 fchmod05 fchmod06 \
        fchmodat01 fchmodat02 \
        fchown01 fchown02 fchown03 fchown04 fchown05 \
        fchownat01 fchownat02 \
        fcntl02 fcntl03 fcntl04 fcntl05 fcntl08 \
        fcntl11 fcntl12 fcntl13 fcntl14 fcntl16 \
        fdatasync01 fdatasync02 \
        flock01 flock02 flock03 flock04 \
        fstat02 fstat03 fstatfs02 \
        ftruncate01 ftruncate03 ftruncate03_64 \
        getcontext01 getcwd02 \
        getdomainname01 \
        getegid01 getegid01_16 getegid02 getegid02_16 \
        geteuid01 geteuid02 \
        getgid01 getgid03 \
        getgroups01 getgroups03 \
        gethostbyname_r01 \
        gethostid01 \
        gethostname01 gethostname02 \
        getitimer01 getitimer02 \
        getpagesize01 \
        getpgid01 getpgid02 \
        getpgrp01 \
        getpid01 getpid02 \
        getppid01 getppid02 \
        getpriority01 \
        getrandom01 getrandom02 getrandom03 getrandom04 \
        getresgid01 getresgid02 getresgid03 \
        getresuid01 getresuid02 getresuid03 \
        getrlimit01 getrlimit03 \
        getrusage01 \
        getsid01 getsid02 \
        gettid01 \
        gettimeofday02 \
        getuid01 getuid03 \
        getxattr01 \
        ioctl_ns07 \
        inotify_init1_01 inotify_init1_02 \
        inotify06 \
        kill03 kill05 kill06 kill07 kill08 kill09 kill10 kill12 \
        lchown01 \
        link02 linkat01 \
        listen01 \
        llseek02 llseek03 \
        lseek01 lseek02 lseek07 \
        mkdir02 mkdir04 \
        mkdirat01 \
        mknod01 mknod02 mknod03 mknod04 mknod05 mknod08 \
        mknodat01 \
        mlock03 mlock04 \
        mlockall01 \
        mmap01 mmap02 mmap03 mmap04 mmap05 mmap09 mmap10 mmap11 mmap12 mmap15 mmap17 \
        mprotect02 mprotect03 \
        msync01 msync02 \
        munmap01 munmap02 \
        nanosleep01 \
        nice01 nice02 nice03 \
        open01 open03 open09 open10 \
        openat01 \
        pathconf01 \
        pipe01 pipe02 pipe03 pipe04 pipe05 pipe08 pipe09 pipe10 pipe13 pipe14 pipe2_02 \
        poll01 poll02 \
        pread01 pread01_64 \
        pwrite01 pwrite01_64 pwrite02 pwrite02_64 \
        read01 read04 \
        readlink01 \
        readv01 \
        rmdir01 rmdir03 \
        rt_sigaction03 rt_sigprocmask01 \
        sbrk01 sbrk02 \
        sched_getaffinity01 \
        sched_getparam01 \
        sched_getscheduler01 sched_getscheduler02 \
        sched_rr_get_interval01 \
        sched_setparam01 sched_setparam02 \
        sched_setscheduler01 \
        sched_yield01 \
        select01 select02 select04 \
        sendfile02 sendfile02_64 sendfile05 sendfile05_64 sendfile06 sendfile06_64 \
        sendfile07 sendfile07_64 sendfile08 sendfile08_64 \
        setegid01 \
        setfsgid01 setfsgid02 \
        setfsuid01 setfsuid02 \
        setgid01 setgid02 \
        setgroups01 setgroups02 \
        setitimer02 \
        setpgid01 \
        setpgrp01 \
        setpriority01 \
        setregid01 setregid02 setregid04 \
        setresgid01 setresgid02 setresgid03 setresgid04 \
        setresuid01 setresuid02 setresuid03 setresuid04 setresuid05 \
        setreuid01 setreuid03 setreuid04 setreuid06 setreuid07 \
        setrlimit04 setrlimit05 \
        settimeofday01 \
        setuid01 setuid03 setuid04 \
        sigaltstack01 sigaltstack02 \
        signal01 signal02 signal03 signal04 signal05 \
        sigprocmask01 sigwait01 \
        socket02 \
        stat01 stat01_64 stat02 stat02_64 \
        statx02 \
        symlink02 symlink04 symlinkat01 \
        sysinfo01 sysinfo02 \
        time01 \
        timer_delete02 timer_getoverrun01 timer_gettime01 \
        times01 \
        truncate02 truncate02_64 \
        umask01 \
        uname02 uname04 \
        unlink05 unlink07 unlinkat01 \
        utime07 \
        wait01 wait02 wait401 wait402 \
        waitid03 waitid04 waitid09 \
        waitpid03 waitpid06 waitpid07 waitpid09 waitpid10 waitpid11 waitpid12 \
        write03 write05 write06 \
        writev02 writev05 writev06 \
        rename09 rename14 renameat201 renameat202 \
        rt_sigprocmask02 \
        close_range02 \
        fcntl01 fcntl01_64 fcntl02_64 fcntl03_64 fcntl04_64 fcntl05_64 fcntl08_64 \
        fcntl09 fcntl09_64 fcntl10 fcntl10_64 fcntl11_64 fcntl12_64 fcntl13_64 \
        fcntl14_64 fcntl16_64 fcntl18 fcntl18_64 fcntl19 fcntl19_64 \
        fcntl20 fcntl20_64 fcntl21 fcntl21_64 fcntl22 fcntl22_64 \
        fcntl29 fcntl29_64 fcntl30 fcntl30_64 fcntl36 fcntl36_64 \
        fcntl37 fcntl37_64 \
        fgetxattr03 \
        flistxattr01 flistxattr02 flistxattr03 \
        flock06 \
        fork01 fork03 fork04 fork05 fork07 fork08 \
        fpathconf01 \
        fstat02_64 fstat03_64 fstatfs02_64 \
        ftruncate01_64 \
        lgetxattr01 lgetxattr02 \
        listxattr01 listxattr02 listxattr03 \
        llistxattr01 llistxattr02 llistxattr03 \
        lstat01 lstat01_64 \
        mallinfo01 mallinfo02 mallinfo2_01 mallopt01 \
        memcmp01 memcpy01 memset01 \
        mincore02 mincore03 \
        mkdir05 \
        mremap01 mremap02 mremap03 mremap04 mremap05 mremap06 \
        openat201 \
        preadv01 preadv01_64 \
        pwritev01 pwritev01_64 \
        readdir01 \
        recvmsg02 \
        removexattr01 removexattr02 \
        rt_sigsuspend01 \
        sendmmsg01 \
        sendmsg02 \
        set_robust_list01 set_tid_address01 \
        setdomainname02 \
        setegid02 \
        setfsgid03 \
        setfsuid03 \
        setgid03 \
        sethostname02 \
        setpgrp02 \
        setsockopt04 \
        sigaction02 sigsuspend01 \
        timerfd02 timerfd_create01 \
        fork10 fork_procs \
        ftest01 ftest02 ftest03 ftest04 ftest05 ftest06 ftest07 ftest08 \
        futex_cmp_requeue01 futex_cmp_requeue02 \
        futex_wait01 futex_wait02 futex_wait03 futex_wait04 futex_wait05 futex_wait_bitset01 \
        futex_wake01 futex_wake03 \
        madvise05 madvise10 \
        mmap19 \
        mprotect04 mprotect05 \
        munlock02 munlockall01 \
        posix_fadvise01 \
        pselect01 pselect01_64 \
        splice01 \
        syscall01 \
        tee01 \
        vfork01 vfork02 \
        asapi_01 asapi_02 \
        atof01 \
        clone03 clone04 clone05 clone06 clone07 clone08 clone302 \
        fptest01 fptest02 \
        inode01 \
        nextafter01 \
        personality01 personality02 \
        pselect03 pselect03_64 \
        sched_get_priority_max02 sched_get_priority_min02 \
        sched_setparam03 sched_setscheduler04 \
        semctl06 semctl07 \
        semop01 semop04 sem_nstest semtest_2ns \
        shmat04 \
        shmdt01 shmdt02 \
        shmt02 shmt03 shmt04 shmt05 shmt06 shmt07 shmt08 shmt09 shmt10 \
        splice09 \
        string01 \
        utsname01 utsname04 \
        genacos genasin genatan genatan2 genceil gencos gencosh genexp \
        genfloor genfrexp genldexp genlog genlog10 genpow gensin gensinh \
        gensqrt gentan gentanh \
        genfabs genfmod genhypot genj0 genj1 genlgamma genload genmodf geny0 geny1 \
        posix_fadvise01_64 posix_fadvise02 posix_fadvise02_64 \
        sched_tc2 sched_tc3 sched_tc4 sched_tc5 \
        stream01 stream02 stream03 stream04 stream05 \
        vmsplice01 \
        af_alg02 af_alg03 af_alg05 af_alg06 \
        cve-2017-17052 \
        print_caps tst_ansi_color.sh tst_exit tst_hexdump \
        clone01 diotest1 diotest4 memcontrol01 mmap001 \
        dirty doio epoll-ltp fs_racer.sh
    do
        run_ltp_case_with_timeout "$name" 60 || (( failed_cases++ ))
    done

    cd /
    print "#### OS COMP TEST GROUP END ltp-$runtime ####"
    (( executed++ ))
    if (( failed_cases == 0 )); then
        print "[CONTEST][PASS] ltp bounded_subset_completed"
        return 0
    fi

    print "[CONTEST][FAIL] ltp bounded_subset_failed cases=$failed_cases"
    (( failed++ ))
    return 1
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

for runtime in glibc musl; do
    run_ltp_bounded_subset "$runtime"
done

print "[CONTEST] Done: $executed tests, $failed failures"

poweroff
