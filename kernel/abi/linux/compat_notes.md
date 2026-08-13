# Linux ABI 兼容性说明

`kernel/abi/linux` 为 A20OS 用户态实现了一组 Linux-compatible syscall 子集。
它是兼容层，不是完整的 Linux kernel personality。
`syscall_table.def` 当前登记 343 个入口（含 2 个 A20OS 扩展与 5 个 x86_64-only
备用槽位 `time`/`pause`/`utime`/`utimes`/`get_thread_area`）。登记表示分派表
覆盖，不表示每项都达到 Linux 语义完整性；已登记的 syscall 可能只支持部分命令、
flag、对象类型或并发边界。表中不再有固定返回 `-ENOSYS` 的显式占位符；仅存的
`-ENOSYS` 返回是架构/版本正确的 Linux 语义（nfsservctl 已在 Linux 4.19 移除、
map_shadow_stack 是 x86 CET、riscv_* 与 arch_prctl 是架构专属）。

## 四主线架构编号覆盖

- **riscv64 / aarch64**：asm-generic 编号 100% 覆盖（仅 `244 arch_specific`
  占位，非真实 syscall）。
- **loongarch64**：补齐 LoongArch 私有 `file_getattr(468)`/`file_setattr(469)`
  （`kernel/fs/vfs/fileattr.c` 核心 + `sys_fileattr.c` 包装）；编号覆盖与
  Linux 一致。
- **x86_64**：`syscall_nr_x86_64.h` 映射表扩展至 463 槽（0–462），修正 ~64 个
  陈旧 `-1` 映射（msgget/msgsnd/msgrcv/msgctl、quotactl、gettid、readahead、
  tkill、set_thread_area、lookup_dcookie、remap_file_pages、restart_syscall、
  fadvise64、utimes、mbind/set_mempolicy/get_mempolicy、mq_*、kexec_load、
  ioprio_*、migrate_pages/move_pages、eventfd→eventfd2、rt_tgsigqueueinfo、
  name_to_handle_at/open_by_handle_at、process_vm_*、kcmp、kexec_file_load、
  mlock2、pkey_*、io_pgetevents、rseq、io_uring_*、open_tree/move_mount/
  fsopen/fsconfig/fsmount/fspick、pidfd_open/pidfd_getfd、process_madvise/
  process_mrelease、epoll_pwait2、mount_setattr、quotactl_fd、landlock_*、
  memfd_secret、futex_waitv、set_mempolicy_home_node、cachestat），并补
  453–462（map_shadow_stack、futex_wake/wait/requeue、statmount、listmount、
  lsm_*、mseal）。新增 x86-only 处理器：`sys_time`（已存在，现注册）、
  `sys_pause`（park 直到信号）、`sys_utime`/`sys_utimes`（vfs_utimensat 的
  timeval/utimbuf 包装）、`sys_get_thread_area`（trap frame 的 FS-base TLS）。
  剩余 `-1` 均为已废弃或 x86-only 遗留（uselib/_sysctl/create_module/
  nfsservctl/iopl/ioperm/modify_ldt/getpmsg 等），返回 `-ENOSYS` 与 Linux 一致。

## 兼容等级

- `full`：目标是在已支持的 flag 和对象类型范围内匹配 Linux 语义。
- `partial`：足够支撑当前用户态和测试，但已知边界语义缺失或被简化。
- `missing`：syscall 未实现，或者关键操作返回 `-ENOSYS`。当前表没有 `missing` 条目；仅存的 `-ENOSYS` 是架构/版本正确的 Linux 语义。

## 当前高风险 Partial 区域

- `bpf(2)`：只支持经 A20OS KEP 引擎验证和执行的 `BPF_PROG_LOAD`、`BPF_PROG_ATTACH`、`BPF_PROG_DETACH`；没有 BPF map 命令，attach 的 `target_fd` 是 A20OS extension-point id，也不等同于 Linux verifier/helper/JIT/BTF/pinning 与对象生命周期语义。
- 调度策略调用：底层是 per-CPU EEVDF、SMP 运行队列、affinity 和远程唤醒；Linux policy/priority/affinity API 仍是有边界的兼容映射，不包含完整的 Linux RT、deadline、cgroup 和 topology 语义。
- Namespace 和 capability 调用：只实现了较小的兼容子集，没有完整的 Linux namespace/security model。
- Futex：已有基础 wait/wake 路径，但高级操作和所有 Linux memory-ordering 边界语义尚不完整。
- VFS：已有很多路径和文件操作，但 mount namespace、symlink、权限、文件系统特定行为和并发语义仍不完整。
- Sockets：AF_INET/AF_UNIX/AF_ALG 兼容面足够覆盖测试，但没有实现完整 Linux network stack 行为。
- POSIX timers 和 timerfd：足够支撑常见等待场景，但 signal delivery 和 overrun 语义被简化。
- Keyring：`kernel/ipc/keyring.c` 实现 add/request/keyctl 的核心命令与 session/user keyring，但没有 key type instantiator、`KEYCTL_*` 全部命令或完整 Linux 安全语义。
- fanotify：基于共享 notify 后端（`kernel/fs/inotify.c`）实现 FAN_CLASS_NOTIF + FID；content/pre-content 类和 per-event fd 不在范围内。
- acct(2)：写入 Linux v3 记账记录，但没有 BSD 风格的 `ac_btime` 高精度或完整字段集合。
- Linux AIO：`kernel/fs/aio.c` 提供上下文与完成队列，但 pread/pwrite/fsync/fdatasync 在当前 VFS 上同步执行，不是后台异步 I/O；`io_cancel` 无法中止正在执行的 op。
- Module syscall：`init_module/finit_module/delete_module` 驱动 A20OS 的 drvmod ET_REL 驱动加载器，加载的是 A20 驱动模块而非 Linux 内核模块，且要求 CAP_SYS_MODULE。
- 跨进程内存：`process_vm_readv/writev` 直接走目标页表（`kernel/mm/process_vm.c`），`process_madvise` 复用 madvise 兼容语义，`process_mrelease` 依赖退出时自动回收。
- mempolicy/NUMA：单 NUMA 节点，`set_mempolicy/mbind/migrate_pages/move_pages` 只做策略校验与存储，不移动物理页。
- 文件句柄：`name_to_handle_at/open_by_handle_at` 基于内核句柄注册表（`kernel/fs/file_handle.c`），不是 filesystem export 操作。
- 新 mount API：`fsopen/fsconfig/fsmount/fspick/open_tree/move_mount/mount_setattr` 建立在现有 mount 表之上；`mount_setattr` 不支持逐 mount 属性。
- io_uring：SQ/CQ 环在内核内存（`kernel/fs/io_uring.c`）并映射进调用者；`io_uring_enter` 同步执行 NOP/READ/WRITE/FSYNC/CLOSE，没有真正的后台异步完成；`IORING_REGISTER_EVENTFD` 在完成时通知已注册的 eventfd。
- userfaultfd（`kernel/ipc/userfaultfd.c`）：MISSING 模式匿名私有区间，UFFDIO_API/REGISTER/UNREGISTER/COPY/ZEROPAGE/WAKE，read/poll 投递 PAGEFAULT 事件，缺页线程 park 直到 handler 用 COPY/ZEROPAGE 解析；不支持 UFFD_FEATURE_EVENT_FORK、shmem 与 WP 模式。
- perf_event_open（`kernel/abi/linux/sys_perf.c`）：PERF_TYPE_SOFTWARE 事件（CPU/TASK clock、page faults、context switches 及无源的软件事件），read(2) 输出 count/time/id，支持 ENABLE/DISABLE/RESET/PERIOD/ID；无 PMU，硬件/raw/breakpoint 事件返回 -EINVAL，无 mmap 采样环。
- futex：全部标准命令（含有边界的 PI 变体 LOCK_PI/UNLOCK_PI/TRYLOCK_PI/WAIT_REQUEUE_PI/CMP_REQUEUE_PI），不携带优先级继承提升；FUTEX_FD（Linux 5.4 移除）返回 -EINVAL。
- Landlock：fd-backed ruleset + path-beneath 规则，在 `vfs_open` 强制；没有完整 LSM 框架。
- rseq：注册/注销每线程 rseq 区域；A20OS 不做跨 CPU 迁移，因此内核从不中止序列。
- `restart_syscall` 返回 `-ERESTARTNOINTR`；`remap_file_pages` 是接受式 no-op；`memfd_secret` 退化为普通 memfd。
- SysV 消息队列（`kernel/ipc/sysv_msg.c`）：固定 32 队列表，支持 msgget/msgsnd/msgrcv/msgctl，含阻塞 park/wake 与 IPC_NOWAIT/MSG_NOERROR。
- POSIX 消息队列（`kernel/ipc/posix_mq.c`）：命名队列 + per-fd mqd，优先级 FIFO，绝对超时（timedsend/timedreceive），mq_notify 用信号投递。
- ioprio/pkey：每任务 I/O 优先级与 16 槽保护键位图（`kernel/proc/sched_compat.c`），pkey_mprotect 等价 mprotect。
- `mseal` 是真实 VMA seal 语义：核心 MM 在 `mm_mmap_locked`/`mm_mprotect_locked`/`mm_munmap_locked`/`mm_mremap_locked`/brk shrink 与 `madvise(DONTNEED/FREE/REMOVE)` 中强制 `VM_SEALED`（覆盖 MAP_FIXED 覆写、mprotect、munmap、mremap 均返回 -EPERM），fork 继承 seal；尚无 `/proc/smaps` 的 Sealed 统计与 userfaultfd 交互。`seccomp` 明确返回不支持而非假装过滤；kexec 明确拒绝。
- `renameat` 已补注册（编号 38，glibc 直接调用）：实现为 `renameat2` 的 flags=0 包装。`sched_setattr`/`sched_getattr` 改为完整 `struct sched_attr` 线格式：校验 policy/flags/nice/priority 并经 `proc_sched_set` 落调度器，支持 `SCHED_FLAG_RESET_ON_FORK`；不应用 util-clamp/deadline 字段。
- `nfsservctl` 返回 -ENOSYS（Linux 4.19 已移除该 syscall，这是正确语义）；`map_shadow_stack` 是 x86 CET 特性，在 RISC-V 返回 -ENOSYS（架构正确）。
- LSM 自省：`lsm_get_self_attr`/`lsm_list_modules` 报告 Landlock；`lsm_set_self_attr` 由 `landlock_restrict_self` 覆盖。
- `statmount`/`listmount`/`listns`/`open_tree_attr` 提供 mount 表自省，`setxattrat` 系列提供 dirfd 相对 xattr。
- RISC-V 专用：`riscv_hwprobe` 报告 IMA 基础行为；`riscv_flush_icache` 刷新范围 icache；`rt_tgsigqueueinfo` 按 tid 投递。
- `time(2)` 在 asm-generic 架构不存在（musl 用 clock_gettime），已从表移除；`SYS_time` 保留仅供 x86_64 架构表映射。

## 文件化接口（/proc、/dev、ioctl）

- `/proc` 新增：`boot_id`、`cap_last_cap`、`nr_open`、`pressure`、`uid_map`/`gid_map`/`setgroups`、`sysvipc`（汇总 msg/sem/shm 计数）、`/proc/sys/kernel/hostname` 与 `domainname`（可读可写，与 sethostname/setdomainname 共享存储）。
- `/dev` 新增：`/dev/full`（读零写 ENOSPC）、`/dev/kmsg`（写追加内核日志环、读回日志；经 `klog_write_raw`/`klog_read`）。
- tty/pty ioctl 补齐：`TIOCGPGRP`/`TIOCSPGRP`（前台进程组）、`TCFLSH`（刷输入/输出/双向）、`TIOCOUTQ`（输出队列字节数）、`TIOCSTI`（注入单字节输入）、`FIONREAD`/`TIOCINQ`（可读字节数）——pty master 与 slave 端均已支持。
- 补充 `core/ioctl.h` 标准 tty ioctl 常量全集（TCSBRK/TCXONC/TIOCEXCL/TIOCM*/TIOCPKT/TIOCGETD 等）供后续驱动与用户态使用。
- `/proc` 经典文件补齐（对照 Uinxed-Kernel）：`devices`、`partitions`、`diskstats`、`modules`（经 drvmod_list）、`misc`、`iomem`、`ioports`、`softirqs`、`route`/`arp`（同时挂在 `/proc` 与 `/proc/net/`）、`tty`、`ldiscs`、`drivers`（经 driver_core_list_drivers）。
- `/proc/net/` 补齐：`route`、`arp`、`dev`、`tcp`、`udp`、`unix`。
- `/proc/thread-self` 目录（与 `self` 同样解析到当前线程）。
- `/proc/<pid>/` 补齐：`limits`、`wchan`、`stack`。

## GPU / 帧缓冲接口

- fbdev 补齐 Linux 标准 ioctl：`FBIOPAN_DISPLAY`（仅接受 (0,0)）、`FBIOBLANK`（接受 0/1 无操作）、`FBIOGETCMAP`/`FBIOPUTCMAP`（真彩帧缓冲返回/接受恒等调色板）。
- 修复 `FBIO_FLUSH`（A20 扩展）与 Linux 标准 `FBIOGETCMAP` 共用 0x4604 的冲突：`FBIO_FLUSH` 移到 0x4609，0x4604 归回 `FBIOGETCMAP`；LVGL 用户态常量同步更新。
- `/sys/class/drm/card0*` 与 `/sys/class/drm/card0-Virtual-1/{enabled,status,modes}` 提供 DRM 显示元数据。
- 音频仍是 A20 原生 `a20_audio` ioctl（非 Linux ALSA SNDRV 标准）；完整 DRM/ALSA 接口集列为后续工作。

## GPU/音频子系统

- **DRM/KMS**（新增 `kernel/drivers/gpu/drm.c`）：`/dev/dri/card0` 提供 Linux 标准 DRM ioctl 子集，映射到 `gpu_dev_ops_t`（virtio-gpu/vmsvga）：VERSION、GET_CAP、GEM_CLOSE、MODE_GETRESOURCES/GETCRTC/SETCRTC/GETCONNECTOR/GETENCODER/GETPLANE/GETPLANERESOURCES/GETFB/ADDFB/RMFB/PAGE_FLIP/DPMS/GETPROPERTY/SETPROPERTY/CREATE_DUMB/MAP_DUMB/DESTROY_DUMB/GETGAMMA/ATOMIC(test)。dumb buffer 用 VMO 支持 mmap。
- **ALSA**（新增 `kernel/drivers/audio/alsa.c`）：`/dev/snd/controlC0`、`/dev/snd/pcmC0D0p`、`/dev/snd/pcmC0D0c` 提供 SNDRV_CTL_IOCTL_PVERSION/CARD_INFO/PCM_NEXT_DEVICE/PCM_INFO 与 SNDRV_PCM_IOCTL_HW_PARAMS/SW_PARAMS/STATUS/WRITEI_FRAMES/READI_FRAMES/PREPARE/START/DRAIN/DROP/PAUSE，映射到 `audio_dev_ops_t`（virtio-snd）。ioctl 路径支持；mmap 播放环列为后续工作。

## 维护规则

1. 新增 Linux syscall 实现必须登记到 `syscall_coverage.md`。
2. 兼容性捷径应在实现里用简短注释标明，并同步反映到本文档。
3. ABI 文件只应负责转换用户参数并调用内部子系统 API；不应拥有子系统全局状态。
4. 如果某个 syscall 为了兼容而故意对未支持特性返回成功，应登记为 `partial` 并在覆盖表注释中说明固定成功行为，不能登记为 `full`。
