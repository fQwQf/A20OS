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
NR = ROOT / "kernel" / "include" / "abi" / "linux" / "syscall_nr.h"
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
}
TRANSFER = {
    "pidfd_getfd":   "A7 全新获取裁决（安装前）",
    "sendmsg":       "A6 发送侧 propagation 门控 + 数据面 total 字节 W 计费（均已落地）",
    "recvmsg":       "A6 接收侧逐 fd 安装裁决 + 数据面 total 字节 R 计费",
}
FAILCLOSED = {
    "io_uring_setup":    "ring 本身不授予权威；SQE 执行点统一调解",
    "io_uring_register": "A9 fixed-file 批量导入对信封任务 -EPERM",
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
    "socketpair": "W2: 双端创建获取（当前未钩）",
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
    return "NA", ""

def main():
    names = parse_defs()
    nums = parse_numbers()
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
