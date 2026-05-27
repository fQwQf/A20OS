# A20OS LTP 测试失败分析与修复 TODO

> 基于 out.txt 测试日志分析。共 215 项 LTP 测试失败，最终在 `float_bessel` 测试时内核崩溃导致后续测试全部未执行。

## 失败统计

| 错误码 | 含义 | 数量 | 说明 |
|--------|------|------|------|
| 32 (ENOSYS) | 系统调用未实现 | ~116 | 大量 syscall 缺失 |
| 1 | 功能不可用 | ~20 | IPv6、cgroup 等 |
| 2 (ENOENT) | 文件/路径不存在 | ~20 | 设备文件、子进程缺失 |
| 6 (ENXIO) | 设备不可用 | ~30 | 缺少块设备 |
| 127/128/129 | 命令未找到/子进程异常 | ~5 | 测试基础设施问题 |
| 崩溃 | SIGSEGV pc=0x0 | 1 | 终止全部后续测试 |

---

## P0 — 致命问题（导致崩溃/测试中断）

### 🔴 1. 用户态空指针跳转导致内核崩溃（float_bessel 测试）
- **现象**: `SIGSEGV: pid=655 code=12 sepc=0x0 stval=0x0 abi=0`，进程跳转到 PC=0x0
- **影响**: QEMU 终止，从 float_bessel 开始的所有后续测试（字母 f 之后的 g-z 全部）均未执行
- **位置**: `kernel/core/trap.c:134-144`
- **根因分析**:
  - `sepc=0x0` 表明进程通过一个 NULL 函数指针进行了函数调用（如 `jalr x0, 0(x0)` 或类似）
  - 可能是用户态库（musl）中的函数指针未初始化，或信号处理/库初始化时出错
  - 也可能是 exec 加载 ELF 时 `.got` / `.plt` 未正确映射
- **修复方向**:
  - [x] 排查 `float_bessel` 测试在 Linux 上的行为，确认其是否依赖浮点扩展
  - [x] 检查 RISC-V 的浮点状态（`sstatus.FS` 字段）是否在 exec/上下文切换中正确初始化和恢复
  - [ ] 在 `kernel/proc/exec.c` 的 ELF 加载路径中增加对 `.got`/`.plt` 段映射的验证
  - [ ] 在 trap_handler 的 SIGSEGV 路径中，当 sepc=0x0 时打印更多诊断信息（上次 syscall 号、寄存器快照）
  - [x] 考虑在 `kernel/arch/riscv64/` 中检查 FPU 上下文切换是否正确保存/恢复
- **已实施**:
  - 在 `trap_frame.h` 中添加了 `f[32]`、`fcsr`、`_pad` 字段到 `trap_context_t`（总大小 560 字节）
  - 在 `trap.S` 中实现了 FPU 保存/恢复：`__trap_from_user` 保存 f0-f31+fcsr，`__return_to_user` 恢复
  - 内核模式下设置 `FS=OFF` 防止意外 FPU 使用破坏用户态状态
  - `exec.c` 中 exec 时清零 FPU 字段（新程序获得干净 FPU 状态）

### 🔴 2. 内核态访问用户地址 0x40 反复触发页错误（30+ 次）
- **现象**: `Kernel failed to access user address. code=13`，`stval(BADV)=0x40`，`sepc` 始终为同一地址
- **影响**: 导致 access02、acct02、aslr01、bind06、cve-2016-10044、cve-2017-2671、clock_gettime01、clock_nanosleep01、fanotify22、fcntl38/39、fgetxattr01 等测试失败
- **位置**: `kernel/core/trap.c:218-239`（kernel_trap_handler）
- **根因分析**:
  - `BADV=0x40` 是一个固定偏移量，说明内核在解引用某个用户态结构体指针+0x40 偏移
  - `code=13`（RISC-V store page fault）说明是写操作触发的
  - 可能是 `copy_to_user()` / `copy_from_user()` 实现在地址验证前就尝试了访问
  - 也可能是某个 syscall handler 直接解引用了用户指针而未做 `access_ok()` 检查
- **修复方向**:
  - [x] 通过反汇编 `.kernel-build/` 中的内核 ELF，定位 `sepc=0xffffffc08020107a` 对应的源码位置
  - [x] 在 `kernel_trap_handler` 的 kernel page fault 路径中打印 backtrace（栈回溯）
  - [ ] 审查所有 syscall handler 中对用户指针的访问，确保都经过 `copy_from_user`/`copy_to_user`
  - [ ] 检查 `kernel/include/sys/usercopy.h` 或相关文件中的用户地址验证逻辑
- **已实施**:
  - 在 `kernel/core/trap.c` 中添加了 `dump_kernel_backtrace()` 函数（栈帧回溯，最多 16 层）
  - 在 KERNEL PAGE FAULT 和 KERNEL OOPS 路径中自动打印 backtrace
  - 诊断已定位故障到 `vsnprintf` 中的 `%s` 处理（用户态无效指针 0x40）
  - **HAL 合规修复**: 将 RISC-V 特定栈回溯移至 `arch/*/include/cpu.h` 中的 `arch_unwind_frames()` + `struct backtrace_frame`
  - 添加 `TRAP_CTX_FP(ctx)` 宏到所有三个架构的 `trap_frame.h`（RISC-V: x[8]/s0, LoongArch: regs[22]/$fp, AArch64: x[29]）
  - 修复了 SP→FP 的 bug：`dump_kernel_backtrace` 现在使用 `TRAP_CTX_FP(ctx)` 而非 `TRAP_CTX_SP(ctx)`
  - `trap.c` 中的 `dump_kernel_backtrace()` 现为纯架构无关代码，通过 HAL 接口调用 `arch_unwind_frames()`

---

## P1 — 高优先级（影响大量测试）

### 🟠 3. /dev/shm 挂载问题（影响 20+ exec 相关测试）
- **现象**: `open(/dev/shm,2,0000) failed: errno=EISDIR(21): Is a directory`
- **影响**: creat07_child、dirtyc0w_shmem_child、execl01_child、execle01_child、execlp01_child、execv01_child、execve01_child、execve06_child、execveat_child、execveat_errno、execvp01_child 等
- **位置**: `kernel/fs/vfs.c:1543-1567`
- **根因分析**:
  - 先 `vfs_mkdir("/dev/shm")` 创建了一个普通目录（在 rootfs 上）
  - 再挂载 devfs 到 `/dev`，这会覆盖整个 `/dev` 目录树，可能导致之前创建的 `/dev/shm` 被遮蔽
  - 然后挂载 tmpfs 到 `/dev/shm`，但如果 devfs 不支持在其挂载点下再挂载子目录，则挂载可能静默失败
  - 最终 `/dev/shm` 仍然是一个空目录而非 tmpfs
- **修复方向**:
  - [x] 调整初始化顺序：先挂载 devfs 到 `/dev`，再挂载 tmpfs 到 `/dev/shm`
  - [x] 在 devfs 挂载完成后创建 `/dev/shm` 目录（而非之前）
  - [ ] 验证 `ramfs_mount_empty()` 返回值，失败时打印错误
  - [ ] 增加 `/dev/shm` 挂载后的验证：尝试在其上创建并删除一个文件
  - [x] 考虑在 devfs 中添加对 `/dev/shm` 的特殊处理
- **已实施**:
  - 在 `devfs.c` 中添加了 `DEVFS_SHM_DIR` 枚举、`g_nodes` 条目、readdir 条目
  - 移除了 `vfs_init()` 中过早的 `vfs_mkdir("/dev/shm")`
  - `/dev/shm` 现在由 devfs 注册，然后 tmpfs 挂载到该路径

### 🟠 4. 块设备/loop 设备缺失（影响 30+ 测试）
- **现象**: `tst_device.c:354: TBROK: Failed to acquire device`
- **影响**: fallocate01-06、fanotify01/03/05/06/09/10/13-21、fdatasync03、close_range01、copy_file_range01/02 等
- **根因分析**:
  - LTP 测试框架需要 `/dev/loopX` 或类似的块设备来创建测试文件系统
  - 当前内核缺少 loop 设备驱动或 loop 设备的 ioctl 接口不完整
- **修复方向**:
  - [x] 确认 `kernel/drv/loop.c` 是否实现了完整的 loop 设备功能
  - [x] 检查 `/dev/loop-control`、`/dev/loop0` 等设备节点是否在 devfs 中注册
  - [x] 实现 `LOOP_CTL_ADD`/`LOOP_CTL_REMOVE`/`LOOP_CTL_GET_FREE` ioctl
  - [x] 实现 `LOOP_SET_FD`/`LOOP_CLR_FD`/`LOOP_SET_STATUS64` 等 loop ioctl
- **已实施**:
  - loop0-loop7 已在 devfs 中注册为块设备
  - 添加了 `/dev/loop-control` 字符设备节点
  - 实现了 `loop_control_ioctl()` 支持 LOOP_CTL_GET_FREE/ADD/REMOVE
  - 已有 LOOP_SET_FD/CLR_FD/SET_STATUS64/GET_STATUS64 支持
  - 已有 BLKGETSIZE64/BLKGETSIZE/BLKSSZGET ioctl

### 🟠 5. 大量系统调用未实现（ENOSYS，~116 项）
- **关键缺失 syscall 列表**:

#### 进程/信号相关
- [x] `acct` (sys_acct) — 进程记账（已添加 -ENOSYS stub）
- [ ] `eventfd` / `eventfd2` — 事件文件描述符（注意 `kernel/ipc/eventfd.c` 存在但可能未注册到 syscall table）
- [x] `signalfd4` — 信号文件描述符（已添加 -ENOSYS stub）

#### 文件系统相关
- [x] `fanotify_init` / `fanotify_mark` — 文件系统事件监控（已添加 -ENOSYS stub）
- [x] `fallocate` — 文件空间预分配（已实现，已在 syscall table 注册）
- [ ] `copy_file_range` — 文件间数据拷贝
- [x] `syncfs` — 同步文件系统（已实现 `sys_syncfs()`）
- [ ] `sync_file_range` — 文件范围同步

#### fcntl 扩展命令
- [ ] `F_SETLEASE` / `F_GETLEASE` / `F_BREAK` (fcntl 24-26, 32-33, 38-39)
- [ ] `F_SETSIG` / `F_GETSIG` (fcntl 34-35)
- [ ] `F_SETOWN_EX` / `F_GETOWN_EX`

#### 模块加载
- [x] `init_module` / `delete_module` / `finit_module`（已添加 -ENOSYS stub）

#### 密钥管理
- [x] `add_key` / `request_key` / `keyctl`（已添加 -ENOSYS stub）

#### AIO (异步 I/O)
- [x] `io_setup` / `io_destroy` / `io_submit` / `io_getevents` / `io_cancel`（已添加 -ENOSYS stub）

#### 其他
- [x] `arch_prctl` (x86 only, RISC-V 上应返回 ENOSYS 即可)（已添加 -ENOSYS stub）
- [ ] `bpf` 的更多命令 (BPF_PROG_LOAD 的部分类型)
- [ ] `cacheflush` (RISC-V 上可能不需要)
- [ ] `clock_adjtime` / `clock_settime`
- [ ] `getrandom` (如未实现)
- [ ] `inotify_init1` / `inotify_add_watch` / `inotify_rm_watch` 部分功能
- [x] `userfaultfd` / `perf_event_open`（已添加 -ENOSYS stub）

### 🟠 6. 16位 UID/GID 变体系统调用缺失
- **现象**: chown01_16 ~ chown05_16、fchown01_16 ~ fchown05_16 全部 ENOSYS
- **根因**: 旧式 16 位 UID 系统调用在 64 位平台上通常由 glibc/musl 映射到标准版本
- **修复方向**:
  - [ ] 检查 musl 是否在 64 位平台上将 `_16` 变体映射到标准 syscall
  - [ ] 如果 syscall 号不同，在 syscall table 中添加映射

---

## P2 — 中优先级（功能缺失/不完整）

### 🟡 7. /etc/group 缺少 daemon 组
- **现象**: `getgrnam(daemon) failed: SUCCESS (0)`
- **影响**: chmod07、fchmod02
- **位置**: `kernel/fs/vfs.c:1582-1583`（仅定义了 root 和 nobody）
- **修复方向**:
  - [x] 在 `/etc/group` 中添加 `daemon:x:1:` 条目
  - [x] 在 `/etc/passwd` 中添加 `daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin`
- **已实施**: 在 `vfs.c` 的 `vfs_init()` 中添加了 daemon 用户和组的条目

### 🟡 8. PTY/pts 子系统缺失
- **现象**: `pty creation failed: ENOENT (2)`
- **影响**: cve-2014-0196 及其他 PTY 相关测试
- **修复方向**:
  - [x] 在 devfs 中注册 `/dev/ptmx` 设备
  - [x] 实现 `ptmx_open`（通过 devpts 获取 pty 从设备）
  - [x] 实现 `/dev/pts/` 目录的自动挂载和设备节点创建
  - [x] 实现 PTY 的 ioctl（TIOCGPTN、TIOCSPTLCK 等）
- **已实施**:
  - 在 `kernel/drv/pty.c` 中实现了完整的 PTY 子系统（master/slave ring buffer 对）
  - 在 devfs 中注册了 `/dev/ptmx`（DEVFS_PTMX）和 `/dev/pts/0`-`/dev/pts/7`（DEVFS_PTS）
  - 支持 TIOCGPTN、TIOCSPTLCK、TIOCGPTP、TIOCGWINSZ、TIOCSWINSZ、FIONBIO 等 ioctl
  - master 和 slave 各自独立的 ring buffer（4KB），支持读写分离

### 🟡 9. 网络功能缺失
- **现象**: add_ipv6addr 失败、ICMPv4/v6 连通性检查失败
- **影响**: add_ipv6addr、check_icmpv4_connectivity、check_icmpv6_connectivity、net 相关测试
- **修复方向**:
  - [ ] 实现 IPv6 地址配置接口（SIOCSIFADDR for IPv6）
  - [ ] 实现 ICMPv6 Echo Request/Reply 处理
  - [ ] 确保内核网络栈（lwip）的 IPv6 已正确初始化

### 🟡 10. cgroup 子系统不完整
- **影响**: cfs_bandwidth01、cgroup_core01/02、cgroup_regression_getdelays、cgroup_xattr、cpuset01、cpuset_syscall_test、cpuset_memory_pressure、cpuset_sched_domains_check
- **修复方向**:
  - [ ] 检查 `kernel/fs/cgroupfs.c` 和 `kernel/proc/cg_cpu.c`、`kernel/mm/cg_mem.c` 中已实现的功能
  - [ ] 补充缺失的 cgroup v1/v2 控制器接口
  - [ ] 实现 cpuacct、cpuset、memory 子系统的核心功能
  - [ ] 确保 cgroup 文件系统支持 xattr 操作

### 🟡 11. Linux Capabilities 不完整
- **影响**: cap_bounds_r/w、cap_bset_inh_bounds、check_keepcaps、check_pe、check_simple_capset、exec_with_inh、exec_without_inh
- **修复方向**:
  - [ ] 确保 `capget`/`capset` syscall 正确实现
  - [ ] 实现 capability bounding set 管理
  - [ ] 实现 exec 时的 capability 继承逻辑
  - [ ] 检查 `kernel/proc/exec.c` 中的 `proc_apply_exec_creds()` 是否处理 capability

### 🟡 12. clone 功能不完整
- **影响**: clone08 (exit 127)、clone09 (TBROK)、clone301、clone303
- **修复方向**:
  - [ ] 检查 `CLONE_NEWNET`、`CLONE_NEWNS` 等 namespace 标志的处理
  - [ ] clone09 失败是因为 `/proc/sys/net/ipv4/conf/lo/tag` 不存在 — 需要完善 procfs 的网络参数暴露
  - [ ] 检查 `kernel/proc/fork.c` 中对各种 clone flags 的支持

### 🟡 13. VIRTIO2 (SD Card) 探测失败
- **现象**: `[VIRTIO2] Probe failed`
- **位置**: `kernel/drv/virtio_blk.c` 或相关驱动
- **影响**: 第二块 virtio-blk 设备（SD 卡镜像）不可用
- **修复方向**:
  - [ ] 检查 virtio_mmio 总线枚举是否正确处理多个设备
  - [ ] 确认 QEMU 启动参数中 `bus=virtio-mmio-bus.1` 对应的地址映射
  - [ ] 在 virtio_blk 驱动中增加对多设备实例的支持

---

## P3 — 低优先级（边界情况/优化）

### 🟢 14. FAT32 磁盘空间不足
- **现象**: `creat05: ENOSPC (28)` — `FAT32_IMAGE_MB=128` 空间不足
- **影响**: creat05 及其他大量创建文件的测试
- **修复方向**:
  - [x] 增大 FAT32 镜像大小（如 256MB 或 512MB）
  - [ ] 在测试脚本中清理临时文件
- **已实施**: FAT32_IMAGE_MB 和 EXT4_IMAGE_MB 默认值从 32 增加到 128

### 🟢 15. 短写（Short Write）问题
- **现象**: `fcntl34.c:66: TBROK: short write(35,...,4096) return value 2224`
- **影响**: fcntl34、fcntl34_64
- **根因**: 内核在写入文件时返回了部分写入而非完整写入
- **修复方向**:
  - [ ] 检查 `kernel/fs/vfs.c` 中 VFS write 路径的返回值逻辑
  - [ ] 确保阻塞写操作会等待直到所有数据写入完成
  - [ ] 可能与 page cache 或 block write 的缓冲管理有关

### 🟢 16. sigtimedwait 返回 EAGAIN
- **现象**: `fcntl31.c:338: sigtimedwait() failed.: errno=EAGAIN`
- **影响**: fcntl31、fcntl31_64
- **根因**: 信号在 sigtimedwait 等待之前就已经到达并丢失
- **修复方向**:
  - [ ] 检查 `rt_sigtimedwait` syscall 实现
  - [ ] 确保待处理信号在调用 sigtimedwait 时被正确检查
  - [ ] 验证信号投递与 sigtimedwait 之间的竞态条件处理

### 🟢 17. access02 执行权限检查失败
- **现象**: `access02.c:129: TFAIL: execute file_x as root failed: SUCCESS (0)`
- **根因**: `access()` 在检查可执行文件时，内核尝试实际执行（通过 `execve`）来验证，但因内核用户地址访问 bug（见 P0-2）而失败
- **修复方向**:
  - [ ] 修复 P0-2（内核用户地址访问 bug）后重新测试
  - [ ] 检查 `sys_access` / `sys_faccessat` 的 X_OK 检查逻辑

### 🟢 18. fcntl07 exec 失败
- **现象**: `fcntl07.c:128: exec failed` — 所有 4 个子测试均失败
- **修复方向**:
  - [ ] 检查 fcntl F_SETLK/F_GETLK 相关的锁继承逻辑
  - [ ] 确认 exec 后文件锁的行为是否符合 POSIX

### 🟢 19. asapi_03 IPV6_RECVPKTINFO 失败
- **现象**: `IPV6_RECVPKTINFO recvmsg: errno=EFAULT(14): Bad address`
- **修复方向**:
  - [ ] 检查 `recvmsg` 对 IPV6_PKTINFO 辅助数据的处理
  - [ ] 确保内核正确填充 `struct cmsghdr` 和 `struct in6_pktinfo`

### 🟢 20. autogroup01 超时
- **现象**: `tst_checkpoint_wait(0, 10000) failed: ETIMEDOUT (110)`
- **修复方向**:
  - [ ] 检查 autogroup 调度功能是否实现
  - [ ] 可能需要实现 `/proc/sys/kernel/sched_autogroup_enabled`

### 🟢 21. clock_gettime01 / clock_nanosleep01 部分失败
- **现象**: 特定时钟类型（如 CLOCK_MONOTONIC_RAW、CLOCK_BOOTTIME）未支持
- **修复方向**:
  - [ ] 在 clock_gettime 中添加 CLOCK_MONOTONIC_RAW、CLOCK_BOOTTIME 等支持
  - [ ] 在 clock_nanosleep 中支持更多时钟类型

### 🟢 22. creat09 ENOSPC / close_range01 设备问题
- **修复方向**:
  - [ ] 增大测试磁盘空间
  - [ ] 实现 close_range syscall（如未实现）

### 🟢 23. dirio / DIO 测试失败
- **现象**: dio_read、dio_sparse、diotest2-6 均失败
- **根因**: O_DIRECT 标志未完整实现或不支持
- **修复方向**:
  - [ ] 在 VFS 层实现 O_DIRECT 读写路径
  - [ ] 确保 O_DIRECT 对齐约束被正确检查

---

## 快速修复优先级排序

按投入产出比排序，修复以下项目可获得最大测试通过率提升：

1. **P0-1 + P0-2**: 修复崩溃 + 内核用户地址访问 → 恢复后半段测试执行 + 修复 30+ 测试
2. **P1-3**: /dev/shm 挂载修复 → 修复 20+ exec 系列测试
3. **P1-4**: loop 设备/块设备 → 修复 30+ fallocate/fanotify 测试
4. **P2-7**: 添加 daemon 组 → 修复 chmod07/fchmod02
5. **P1-5**: 实现 eventfd → 修复 eventfd01-06 共 6 个测试
6. **P2-8**: PTY 子系统 → 修复 PTY 相关测试
7. **P1-5**: 实现 fallocate → 修复 fallocate01-06
8. **P2-10/11**: cgroup/capability 完善 → 修复 20+ 测试

---

## 备注

- 测试在 RISC-V QEMU 上运行，部分 x86 专用测试（arch_prctl01、f00f）的失败是预期行为
- LTP 版本: 20240524
- 已有黑名单文件: `user/contest_init/ltp_blacklist.txt`（跳过了 70+ 已知不稳定/危险的测试）
- VFS 初始化流程: `kernel/fs/vfs.c:1535-1567`
