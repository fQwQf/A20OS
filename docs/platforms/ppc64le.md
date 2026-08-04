# PPC64LE 平台状态

PPC64LE 当前由 QEMU pSeries（`qemu-virt-ppc64le`）提供受支持的 MMU
运行目标。启动入口切换到 64 位 little-endian 模式，使用 PowerPC Radix
页表；用户地址空间保留低半根页表，内核高半映射通过每个进程共享的根项
继承。异常入口同时覆盖用户态系统调用、用户缺页和内核态 timer/device
中断，地址空间切换使用 pSeries process table 的 PID 1。

## 构建与运行

```sh
make ARCH=ppc64le BOARD=qemu-virt-ppc64le ABI=linux BRINGUP=1 kernel-only
make ARCH=ppc64le BOARD=qemu-virt-ppc64le run
make check-ppc64le-bringup
make check-ppc64le-user
```

QEMU 目标使用 pSeries 固件提供的虚拟终端、RTAS、PCI 配置访问和 TCE
DMA 窗口。`poweroff` 和 `reboot` 通过 RTAS token 请求固件动作；virtio
块设备和网络设备通过 pSeries PCI transport 探测。

## 当前进展（2026-08-04）

已完成并可在干净提交上复现：

- 启动与 Radix MMU 正常：`PAGE_OFFSET` 调整为根索引 256 的高半映射，
  `arch_current_cpu_id()` 在 pSeries guest 中不再读 PIR（会触发 Program
  Exception），`BRINGUP` smoke 通过且 RTAS poweroff 干净退出。
- 完整用户态（musl + 125 个命令 + init + mksh）已构建进 dev 镜像。
- 板级 `enumerate_devices` 直接调用 RTAS PCI/virtio 探测；virtio-blk 挂载
  fat32 到 `/bin`，`/bin/init` ELF 可加载，用户任务（pid 2）可创建。
- 修复了 ISI 取指页错误不保存故障地址的问题：指令页故障的 tval 应为
  SRR0 而不是 DAR，否则 demand paging 永远映射不了入口页。
- **`mksh` 交互 shell 已跑通**：真实 O3 init/mksh 可启动，shell 能执行内建
  `echo`、`ls` 与外部 `/bin/echo`（`fork` + `execve` + COW + `wait` +
  `SIGCHLD` 全链路）；`#` prompt、输入回显、父进程信号返回和阻塞读均正常。
- **native ABI libc（liba20c）已移植并在 QEMU 中运行**：`native-libc-ppc64le`
  `PASS`（字符串/malloc/time/printf 全通过），`native-hello-ppc64le` 与
  `native-mm-ppc64le` 也通过。crt0 补上 ELFv2 TOC 初始化（`_start` 先设 r2
  为 `.TOC.` 基址，native ABI loader 保证 r12=入口地址）；liba20c 的
  `printf` vararg 读取改为经 `va_list *` 推进，修复 `%d` 后 `%s` 等混合
  宽度格式错位（该 bug 影响所有架构，ppc64le 的 -O0 构建使其最先暴露）。

## 已知边界与阻塞

- **用户态 shell 已跑通**（`mksh` 交互 + `fork/exec` 外部命令），关键
  PPC64LE 架构修复如下：
  1. **低向量布局**：`0x300/0x380/0x400/0x480` Book3S 槽只有 0x80 字节，
     完整异常体不再直接覆盖；改为 4 字节绝对跳转 stub，完整 DSI/DSEG/ISI
     trampoline 复制到 scratch 页内的 `0x1100/0x1200/0x1300`。旧版把完整
     向量直接复制进窄槽，导致向量互相覆盖（DSI 串入后续槽并最终被写到
     `0x500`），这是之前误判为“QEMU 0x500 破坏 SRR0”的真实根因。
  2. **VMX/VPU 上下文**：musl/GCC 的 `__setjmp_toc` 会执行 `stvx` 保存
     v20-v31。新增 `0xf20` VPU-unavailable stub（`0x1400` trampoline），
     `trap_bridge.c` 在首次 VPU trap 时设置 `MSR_VEC`（lazy enable）；
     `trap_context_t` 扩到 848 字节并在 trap 入口/返回按 `MSR_VEC` 保存/
     恢复 v0-v31（`trap.S`）。此前没有 VPU 支持，首个 `stvx` 立即死于
     `0xf24`。
  3. **上下文切换丢失地址空间**：`__switch` 保存 `task_context_t` 时未写
     偏移 176 的 `pgdir`，恢复时 `switch.S` 把未初始化栈内容装进 PID 1
     process-table entry，切回被 park 的任务后用户页表根被破坏，`read`
     syscall 的下一条指令（如 `0x73590`）无限 ISI。修复：切出时把
     `ppc64_current_addr_space` 存入 `pgdir` 槽。
  4. **DSI 未区分 load/store**：DSI 向量把所有数据页故障标成
     `CAUSE_LOAD_PAGE_FAULT`，fork 后的 COW 写故障（`std` 到只读页）不会
     走 store/COW 修复，原指令无限重试。修复：按 DSISR 的 ISSTORE 位
     （`andis. 0x0200`）把 store 故障动态标成 `0x380`。
  5. **powerpc64 `struct stat` 布局**：musl powerpc64 的 `nlink_t` 为 64 位，
     `st_nlink@16`、`st_mode@24`、结构总长 144 字节；此前误用 asm-generic
     64LE 布局（`st_mode@16`、128 字节），导致 `[[ -f ]]`/`[[ -x ]]` 判错、
     `exec` 报 `EACCES`。修复：`stat_abi.c` 改为 144 字节 PPC64 专用布局。
  6. **`proc_sched_tick`**：timer IRQ 补上 `proc_sched_tick(from_user)`，
     使 park 的等待任务能按 deadline 被调度检查（与其余架构一致）。
  7. **TTY ioctl ABI 与输入回显**：powerpc64 musl 使用 `_IOC` 编码的
     `TIOCGWINSZ/TCGETS/TCSETS`，而内核此前只接受 asm-generic 编号，导致
     `isatty()` 失败、mksh 不进入交互模式。内核现兼容 PPC64 请求号和
     44 字节 termios 布局，并按 `ECHO` 回显 console 输入。
  8. **跨任务 trap stack 归属**：低向量曾从共享 scratch 读取 kernel stack
     top；子进程运行后会覆盖该值，使父进程下一次 syscall 把 trap frame
     写到子进程内核栈。调度切换和用户返回现通过每 CPU `SPRG2` 发布当前
     任务 kernel stack，异常入口不再使用跨任务共享槽。
  9. **SIGCHLD/sigreturn**：PPC64 trampoline 原来使用旧 syscall `172`，但
     本项目 powerpc64 musl 使用 asm-generic `rt_sigreturn=139`，实际会误调
     `getpid` 后执行栈上零字节。trampoline 已改为 139；signal mcontext 同时
     保存/恢复 LR、CTR、XER、CR，sigreturn 也不再套用普通 syscall 的 CR0
     返回值改写。
  10. **异常编号拆分**：program exception、FP unavailable 和 emulation
      assist 不再共用 `0x700`；scalar FP unavailable 现在按 `0x800` 独立
      lazy-enable，避免合法浮点指令被误报为用户程序异常。
  - **历史修复（已保留，不再引用为当前阻塞）**：
    - 2026-08-03 `__switch` 任务上下文与 C 帧碰撞导致内核跳 0：`switch.S`
      先把 `task_context_t` 分配在当前栈下方（同 loongarch64），保存后
      `task->kstack` 指向该位置，恢复时 `sp` 仍取原帧基址。
    - 2026-08-03 用户态突破：`PTE_U` 与硬件 EAA_PRIV 位冲突已修复
      （`PTE_U` 移到软件位 bit 49，`PTE_PRIV` 移到 bit 59）；`handle_present_page_fault`
      对 present 页设置 `PTE_A`；`arch_tlb_flush_page` 回退到完整
      `arch_tlb_flush`（address-form tlbie 在 TCG 下不可靠）。
    - 2026-08-03 RPDB：进程表条目不再对 `PRTBE_R_RPDB` 做字节交换，直接
      用 `(root_pa & 0x0FFFFFFFFFFFFF00) | 13`。
    - **过期结论，不再引用**：早期把存储故障投递解释为“QEMU 0x500 破坏
      SRR0/nip”并用 `ppc64_rtu_r1/r2/nip` 恢复的说法已被推翻——真实根因
      是本移植低向量布局错误（完整向量覆盖 0x300-0x4ff 窄槽），相关
      `trap_bridge.c`/`trap.S` 恢复逻辑已删除。musl mallocng/GCC 误编译的
      调查报告也已过期，`mksh` 现可完整运行。
- NOMMU 不支持，SMP 平台启动和远程 TLB shootdown 尚未加入已验证矩阵；
  构建系统默认拒绝该架构的 NOMMU 配置，SMP 实验必须显式设置
  `ALLOW_UNVERIFIED_SMP=1`。
- native ABI 其余子系统测试尚未全绿（均与 libc 移植无关，属内核逻辑/功能
  缺口）：`native-handle-ppc64le` 在 `dup-write-denied` 处失败（dup 降级后的
  句柄写入未按预期被拒）；`native-futex-ppc64le` 报 `did not return
  WOULDBLOCK`；`native-signal-ppc64le` 在 worker 线程的 `signal_check`
  检查点触发 SIGSEGV（native 线程/信号路径）。
