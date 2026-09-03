#!/usr/bin/env python3
"""Generate the capability-envelope choke-point coverage matrix.

Implements docs/research/08 §4 row 1 ("咽喉完备性") mechanically: every
LINUX_SYSCALL registered in kernel/abi/linux/syscall_table.def is
classified against the mediation contract of docs/research/05 §2.5, so a
newly added syscall cannot silently bypass env_mediate_* -- the gate
(check-envelope-coverage) fails until it is classified.

Classes:
  ACQUIRE    creation-time acquisition, mediated (05 §2.5.1 A1-A3/A5/A7)
  TRANSFER   descriptor-transfer events, mediated (A6/A7)
  USE        authority consumption, direction-aware (read/write family,
             io_uring SQE execution point)
  FAILCLOSED denied outright for enveloped tasks until designed (A9/A10)
  PLANNED    known unmediated surface, scheduled W2 with reason
  NA         no resource authority involved

Output: docs/research/verification/envelope_coverage.md
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEF = ROOT / "kernel" / "abi" / "linux" / "syscall_table.def"
NR = ROOT / "kernel" / "include" / "core" / "syscall_nr.h"
OUT = ROOT / "docs" / "research" / "verification" / "envelope_coverage.md"

# ---------------------------------------------------------------- curation
# Every non-trivial classification cites the design section that owns it.
ACQUIRE = {
    "openat":        "A1 权利推导 + A8 重开权利交集",
    "socket":        "A2 网络 rights + 类上限",
    "pipe2":         "A3 两端各一影子",
    "memfd_create":  "A3 匿名 FILE",
    "eventfd2":      "A3 EVENT_QUEUE",
    "timerfd_create":"A3 TIMER",
    "signalfd4":     "A3 EVENT_QUEUE（仅创建分支）",
    "shmat":         "A5 MEMORY 类检查（足迹计费 W2）",
    "accept4":       "A2 派生 socket 全新获取裁决",
    "accept":        "同 accept4（薄委托）",
    "socketpair":    "A2 双端 SOCKET 获取，各自安装影子",
    "io_uring_setup":"A9 ring fd 按 EVENT_QUEUE 获取；SQPOLL 拒绝",
    "a20_channel_pair": "Linux bridge 双端 CHANNEL_ENDPOINT 获取",
    "a20_registry_client": "Linux bridge registry CHANNEL_ENDPOINT 获取",
}
TRANSFER = {
    "pidfd_getfd":   "A7 全新获取裁决（安装前）",
    "sendmsg":       "A6 发送侧 propagation 门控 + 数据面 total 字节 W 计费（均已落地）",
    "recvmsg":       "A6 接收侧逐 fd 安装裁决 + 数据面 total 字节 R 计费",
}
FAILCLOSED = {
    "io_uring_register": "A9 REGISTER_FILES 分支对信封任务 -EPERM；EVENTFD 完成通知在执行点调解",
}
USE = {
    "read":      "方向位 R", "readv": "方向位 R", "pread64": "方向位 R",
    "write":     "方向位 W", "writev": "方向位 W", "pwrite64": "方向位 W",
    "io_uring_enter": "A10 SQE 执行点 READ/WRITE/FSYNC 调解",
    "connect":   "socket use 计次", "bind": "同 connect", "listen": "同 connect",
    "sendto":    "方向位 W + len 字节计费", "recvfrom": "方向位 R + len 字节计费",
    "sendmmsg":  "经 sendmsg_from_msghdr 汇聚计费",
    "recvmmsg":  "经 recvmsg 汇聚计费", "recvmmsg_time64": "同 recvmmsg",
}
# Known unmediated surfaces, each owned by a W2 line item.
PLANNED = {
    # socket control-plane odds and ends
    "setsockopt": "W2 审计", "shutdown": "W2",
    # memory mappings (A4 设计已写、实现未接)
    "mmap": "W2: A4 file-backed Map right + 时间",
    # SysV / POSIX IPC
    "shmget": "W2: 段创建类检查", "shmctl": "W2 审计",
    "semget": "W2", "semop": "W2", "semtimedop": "W2", "semtimedop_time64": "W2", "semctl": "W2",
    "msgget": "W2", "msgsnd": "W2", "msgrcv": "W2", "msgctl": "W2",
    "mq_open": "W2", "mq_timedsend": "W2", "mq_timedsend_time64": "W2",
    "mq_timedreceive": "W2", "mq_timedreceive_time64": "W2",
    # other authorities
    "epoll_create1": "W2 EVENT_QUEUE", "inotify_init1": "W2 EVENT_QUEUE",
    "bpf": "W2: 建议 ENV 直接拒绝", "userfaultfd": "W2",
    "perf_event_open": "W2", "fanotify_init": "W2",
    "mount": "W2: 特权挂载对信封拒绝", "umount2": "W2",
    "swapon": "W2", "swapoff": "W2",
    "ptrace": "W2: 跨进程内省对信封拒绝", "process_vm_readv": "W2 同 ptrace", "process_vm_writev": "W2 同 ptrace",
    "open_tree_attr": "W2 mount-api 族", "listmount": "W2 mount-api 族", "statmount": "W2 mount-api 族",
}

# Syscalls with no resource-authority acquisition, transfer, or consumption
# under the envelope contract.  This allowlist is intentionally exhaustive:
# a new syscall must be placed in one of the curated sets instead of silently
# falling through to NA.
NA = frozenset("""
io_setup io_destroy io_submit io_cancel io_getevents
setxattr lsetxattr fsetxattr getxattr lgetxattr fgetxattr listxattr
llistxattr flistxattr removexattr lremovexattr fremovexattr getcwd
lookup_dcookie epoll_ctl epoll_pwait dup dup3 fcntl inotify_add_watch
inotify_rm_watch ioctl ioprio_set ioprio_get flock mknodat mkdirat unlinkat
symlinkat linkat renameat pivot_root nfsservctl statfs fstatfs truncate
ftruncate fallocate faccessat chdir fchdir chroot fchmod fchmodat fchownat
fchown close vhangup quotactl getdents64 lseek preadv pwritev sendfile
select ppoll vmsplice splice tee readlinkat fstatat fstat sync fsync
fdatasync sync_file_range timerfd_settime timerfd_gettime utimensat acct
capget capset personality exit exit_group waitid set_tid_address unshare
futex set_robust_list get_robust_list nanosleep getitimer setitimer
kexec_load init_module delete_module timer_create timer_gettime
timer_getoverrun timer_settime timer_delete clock_settime clock_gettime
clock_getres clock_nanosleep syslog sched_setparam sched_setscheduler
sched_getscheduler sched_getparam sched_setaffinity sched_getaffinity
sched_yield sched_get_priority_max sched_get_priority_min
sched_rr_get_interval restart_syscall kill tkill tgkill sigaltstack
sigsuspend sigaction sigprocmask rt_sigpending sigtimedwait rt_sigqueueinfo
sigreturn setpriority getpriority reboot setregid setgid setreuid setuid
setresuid getresuid setresgid getresgid setfsuid setfsgid times setpgid
getpgid getsid setsid getgroups setgroups uname sethostname setdomainname
getrlimit setrlimit getrusage umask prctl getcpu gettimeofday settimeofday
adjtimex getpid getppid getuid geteuid getgid getegid gettid sysinfo
mq_unlink mq_notify mq_getsetattr shmdt getsockname getpeername getsockopt
readahead brk munmap mremap add_key request_key keyctl clone execve
fadvise64 mprotect msync mlock munlock mlockall munlockall mincore madvise
remap_file_pages mbind get_mempolicy set_mempolicy migrate_pages move_pages
rt_tgsigqueueinfo riscv_hwprobe riscv_flush_icache wait4 prlimit64
fanotify_mark name_to_handle_at open_by_handle_at clock_adjtime syncfs setns
kcmp finit_module sched_setattr sched_getattr renameat2 seccomp getrandom
execveat membarrier mlock2 copy_file_range preadv2 pwritev2 pkey_mprotect
pkey_alloc pkey_free statx io_pgetevents rseq kexec_file_load
clock_gettime64 clock_settime64 clock_getres_time64 clock_nanosleep_time64
timer_gettime64 timer_settime64 timerfd_gettime64 timerfd_settime64
utimensat_time64 pselect6_time64 ppoll_time64 io_pgetevents_time64
rt_sigtimedwait_time64 futex_time64 sched_rr_get_interval_time64
pidfd_send_signal open_tree move_mount fsopen fsconfig fsmount fspick
pidfd_open clone3 close_range openat2 faccessat2 process_madvise
epoll_pwait2 mount_setattr quotactl_fd landlock_create_ruleset
landlock_add_rule landlock_restrict_self memfd_secret process_mrelease
futex_waitv set_mempolicy_home_node cachestat fchmodat2 map_shadow_stack
futex_wake futex_wait futex_requeue lsm_get_self_attr lsm_set_self_attr
lsm_list_modules mseal setxattrat getxattrat listxattrat removexattrat
file_getattr file_setattr listns a20_envelope_create a20_envelope_enter
a20_envelope_revoke a20_envelope_stats a20_envelope_audit arch_prctl
set_thread_area poll time pause utime utimes get_thread_area mkswap shm_open
alarm clock_gettime32
""".split())

def parse_defs():
    txt = DEF.read_text()
    return re.findall(r"^LINUX_SYSCALL\(([a-z0-9_]+)", txt, re.M)

def parse_numbers():
    nums = {}
    for line in NR.read_text().splitlines():
        m = re.match(r"#define SYS_([a-z0-9_]+)\s+(\d+)", line)
        if m:
            nums[m.group(1)] = int(m.group(2))
    return nums

def classify(name):
    if name in ACQUIRE:
        return "ACQUIRE", ACQUIRE[name]
    if name in TRANSFER:
        return "TRANSFER", TRANSFER[name]
    if name in USE:
        return "USE", USE[name]
    if name in FAILCLOSED:
        return "FAILCLOSED", FAILCLOSED[name]
    if name in PLANNED:
        return "PLANNED", PLANNED[name]
    if name in NA:
        return "NA", ""
    raise ValueError(f"unclassified syscall: {name}")

def main():
    names = parse_defs()
    nums = parse_numbers()
    groups = {
        "ACQUIRE": set(ACQUIRE), "TRANSFER": set(TRANSFER),
        "USE": set(USE), "FAILCLOSED": set(FAILCLOSED),
        "PLANNED": set(PLANNED), "NA": set(NA),
    }
    memberships = {}
    for cls, members in groups.items():
        for name in members:
            memberships.setdefault(name, []).append(cls)
    duplicates = {n: cs for n, cs in memberships.items() if len(cs) != 1}
    stale = set(memberships) - set(names)
    if duplicates:
        raise ValueError(f"multiply classified syscalls: {duplicates}")
    if stale:
        raise ValueError(f"classified syscalls absent from table: {sorted(stale)}")

    rows = []
    counts = {"ACQUIRE": 0, "TRANSFER": 0, "USE": 0, "FAILCLOSED": 0,
              "PLANNED": 0, "NA": 0}
    for n in names:
        cls, note = classify(n)
        counts[cls] += 1
        num = nums.get(n, "")
        rows.append((num, n, cls, note))

    order = ["ACQUIRE", "TRANSFER", "USE", "FAILCLOSED", "PLANNED", "NA"]
    out = []
    out.append("# 信封咽喉完备性覆盖矩阵（自动生成，勿手改）\n")
    out.append("由 `tools/gen_envelope_coverage.py` 从 `kernel/abi/linux/"
               "syscall_table.def` 机械生成——每登记一个新 syscall，"
               "`make check-envelope-coverage` 即失败直至其被显式分类。\n")
    out.append(f"- 登记入口总数：**{len(names)}**")
    for k in order:
        out.append(f"- {k}：{counts[k]}")
    cov = counts['ACQUIRE'] + counts['TRANSFER'] + counts['USE'] \
        + counts['FAILCLOSED']
    out.append(f"- **资源权威相关且已调解：{cov}**；"
               f"已知未调解面（PLANNED）：{counts['PLANNED']}（全部挂 W2 行项）；"
               f"无权威参与（NA）：{counts['NA']}\n")
    out.append("分类语义见 docs/research/05 §2.5；类目定义见生成脚本头部。"
               "PLANNED 行项清零是论文投稿前条件（审稿人第一攻击点）。\n")
    out.append("| nr | syscall | 分类 | 说明 |")
    out.append("|------|---------|------|------|")
    for num, n, cls, note in sorted(rows, key=lambda r: (r[0], r[1])):
        out.append(f"| {num} | `{n}` | {cls} | {note} |")
    out.append("")
    OUT.write_text("\n".join(out))
    print(f"envelope_coverage.md: {len(names)} entries, "
          + ", ".join(f"{k}={counts[k]}" for k in order))

if __name__ == "__main__":
    sys.exit(main())
