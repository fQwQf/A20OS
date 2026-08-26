# A20OS 改进 TODO

本文档记录 A20OS 的工程瓶颈和剩余改进工作（最后核实：2026-08）。checkbox 表示实现里程碑，不表示运行结果已在当前提交复验；文中带日期的验证记录均为历史记录，引用规则见文末"验证环境说明"。

## P0：混合内核改造（Native ABI 本体化）

改造定位、边界原则与阶段验收标准见 [../hybrid-kernel/03-refactor-plan.md](../hybrid-kernel/03-refactor-plan.md)。 本节只跟踪各阶段的工程完成度。

- [x] 阶段一：核心原语契约化（句柄 rights 代数、channel 背压、EventQ 语义、VMO 生命周期）。
  - 源码证据：`user/tests/test_native_contract.c` 覆盖 rights、channel、EventQ 和 VMO 生命周期分区；`kernel/abi/native/handle_table.c` 的 `CHANNEL_ENDPOINT`/`EVENT_QUEUE` 类型掩码包含 STAT，handle query 可用。
  - 历史验证：2026-08-06 `smoke-native-contract` 在 riscv64 PASS，loongarch64 构建通过（历史记录，当前状态需复验）。
- [x] 阶段二：Native ABI SMP 正确性收口（native-shmring SMP=2/8 偶发破坏）。
  - 历史验证：SMP=2 连续 20 轮 + SMP=8 连续 20 轮零失败零挂起（2026-08-06，日志 `.kernel-build/smoke/shmring-smp{2,8}/`）；M5 修复 `98a1260`/`1af0d02` 在当时复验有效（历史记录，当前状态需复验）。
  - 副产品：`[VMO-PAGE]` 串口诊断降级为 `/proc/a20/objects` 的 `vmo_dirty_frames` 计数器（合法复用，消除输出交错）；当时还记录了 mm_stress 45s 门禁预算不足。
- [ ] 阶段三：驱动双态部署框架 + IOMMU/DMA 真隔离。
  - 已有骨架：`drv_env.h` 提供 KERNEL/USER/DRVMOD 三后端；virtio-input 的 DRVMOD 只读 probe 与 USER uinputd 共享配置协议头，USER 路径有 virtq/DMA/IRQ→EventQ；完整 DRVMOD 驱动仍使用独立实现。goldfish RTC DRVMOD probe 仍复制寄存器常量，不是同源双态。
  - 过程中修复：`QUEUE_READY` 偏移（0x044）、DRIVER_OK 时序、 vmo_phys 非物化契约（drv_dma 先触页再翻译）、native 构建 stamp 不含 user/svc 与共享头的陈旧二进制问题。
  - 完成条件：同一完整驱动源码双态部署通过同一契约测试；用户驱动 DMA 动态绑定 per-device IOMMU domain，未授权访问产生并消费 fault。当前只有 RISC-V IOMMU bring-up 与 devid 0 静态 TR_REQ 探测；仍缺动态 map/unmap、fault 消费和 drv_dma 接线。
- [x] 阶段四：服务接口 IDL 化（替换 `user/svc/*_proto.h` 手写协议）。
  - 源码证据：绑定槽位（svc/ping/shmring/chand/rtcd/ubd）、shmring 几何与消息布局全部出自 `user/svc/a20_services.idl`，经 `tools/a20idl.py` 生成单一头；四个手写 wrapper（`rtcd_proto.h`、`svc_proto.h`、`ubd_proto.h`、`shmring_proto.h`）已删除出活跃树，native 服务二进制直接依赖 `$(A20_SERVICES_IDL_HDR)`；驱动私有硬件放置常量（goldfish RTC、ubd 的 virtio-mmio 槽位几何）内联回各驱动。版本协商常量 `A20_SERVICES_IDL_VERSION` 由 IDL version 字段生成。
  - 验证：`make check-a20-idl` + `smoke-native-{fs-all,svc,registry,rtcd}` riscv64 PASS；`smoke-native-shmring` 功能基准 PASS 但其 poweroff 路径命中预先存在的 pfa 空闲链损坏（在未改动基线 720e16ab0 上等价复现），跟踪于 MM 小节新条目。
- [ ] 阶段五：Linux 人格层在 Native 原语上重建（starnix 式对照）。
  - 已完成起步：`a20_personality.h` 提供 pipe-shaped channel/EventQ facade， `smoke-native-personality` 验证写入、MESSAGE_READY、读取和关闭。
  - 完成条件：fd 表、byte-stream accumulator、mmap/VMO、socket 等关键 子集在直通实现与人格层实现下同通过，语义 diff 与性能对照归档。

## 用户态文件系统宿主（2026-08 落地，后续项）

已落地：uxfs 内核代理 + ufsd 多人格宿主（fat/ext4 读写、iso9660/ntfs 只读）、svcmgr argv 清单托管、SIGKILL 恢复演练（`smoke-native-fs-all`）。设计见 [../hybrid-kernel/06-user-fs.md](../hybrid-kernel/06-user-fs.md)。

- [x] ufsd 监管通道接入 EventQ 统一等待，替换 200µs 轮询。
  - 源码证据：`user/svc/ufsd.c` 的服务循环把 fs 通道（MESSAGE_READY/PEER_CLOSED）与监管 ping 槽位挂到同一 EventQ，空闲时阻塞在 `event_wait`，先排空后等待的顺序保留"watch 注册前已发布消息也要处理"的契约；ping 槽位未安装的启动方式（如 fs-all 冒烟里的多实例）降级为仅 fs 通道唤醒，与 rtcd 的 `have_ping` 先例一致。`make smoke-native-fs-all`、`smoke-native-svc`、`smoke-native-registry` 在 riscv64 PASS（当前提交）。
- [x] uxfs 文件读写接入内核页缓存（readpage/writepage 缓存路径），替代直通转发。
  - 源码证据：`vfs_file_uses_buffered_write` 纳入 FS_TYPE_UXFS（服务端经 fs_serve flags bit0 声明只读的挂载除外，iso9660 据此声明）；`uxfs_writepage` 尾页按 vn->size 截断防止 ufsd 侧尺寸膨胀；`uxfs_fclose` 关闭时冲刷脏页（ufsd 可崩溃重启，write-behind 不得越过 close）；truncate 走 `page_cache_truncate` 失效缓存；新增 `.sync_vnode`（UFS_OP_SYNC）顺带修复 fsync 把 uxfs_sb 当 bcache 同步的类型隐患；VNFS 后端 `vn_read/vn_write` 改经 lseek 同步后端私有游标，修复页粒度 RPC 非零偏移被忽略的潜在缺陷。
  - 验证：`make smoke-native-fs-all`（含 SIGKILL 重启持久化，即关闭时冲刷的行为验证器）+ `smoke-native-ufs` + `smoke-vfs-stress` riscv64 PASS（当前提交）。
- [x] ntfs 写使能：先为内核 ntfs 写路径补齐在库正确性测试（此前全仓库零覆盖），再打开 ufsd ntfs 后端读写。
  - 源码证据：`user/svc/ufs_vnfs_backend.c` ntfs 分支 `g_readonly=0`；`user/cmds/core/ufs_all_test.c` test_ntfs 覆盖新建写读回/mkdir/嵌套读取/SIGKILL 重启持久化/unlink/rmdir。配套修复的内核 ntfs 缺陷：MFT 记录 IO 丢失簇内偏移（读写都定位到错误记录槽）、USA 数组与首属性重叠导致 fixup 破坏记录、索引项字段布局偏离规范（len/stream_len/flags 错位）、`read_directory` 计数遍历恒返回空、`$INDEX_ROOT`/`$Bitmap` 具名属性匹配失败、驻留 `$INDEX_ROOT`/`$DATA` 属性增长不后移后续属性、空闲记录扫描把未初始化槽位当 IO 错误。
  - 验证：`make smoke-native-fs-all` 在 riscv64 PASS（当前提交）。
- [x] 托管设备序号的板级配置化：清单不再固化 block_index。
  - 源码证据：`user/svc/svcmgr.c` 从 `/proc/cmdline` 读取 `a20.ufsd_blk=<n>`（校验 1..15）动态构造 ufsd 参数串并日志记录解析来源（cmdline/default），缺省回落 QEMU 开发布局的 1；配套修复 `/proc/cmdline` 此前渲染硬编码假值、不暴露真实内核命令行的问题。与 net 的 a20.* 键同一哲学：只认命令行/运行时配置，不做编译期板级表。
  - 验证：`make smoke-native-fs-all` 以 `-append 'a20.ufsd_blk=1'` 启动并在门禁断言 `SVC_MGR: ufsd blk=1 (cmdline)`，riscv64 PASS（当前提交）。

## P0：并发与 SMP 就绪

- [x] 建立 tokenized Park/Wake，消除“检查条件后、真正睡眠前”的丢失唤醒窗口。
  - 证据：`kernel/include/proc/park.h` 的 `A20_PARK_WAKE_PROTOCOL` 与 `kernel/include/core/sync.h` 的 `WAIT_QUEUE_PARK_PROTOCOL`；wait queue、 futex、timeout 和 wake queue 都保存 task 引用及 `wait_seq`。
  - 验证：`make check-blocking-point-boundary`。
- [x] 收口 task 引用与异步所有权。
  - 证据：PID 查询使用 `proc_find_get()`；task list、PID table、runqueue、 dispatch/current、wait/wake 和 timeout owner 都有显式引用交接； `/proc/a20/task_lifetime` 提供基线与错误计数。
  - 验证：`make check-task-lifetime-boundary` 和 `make check-proc-step35-local`。
- [x] 统一信号、停止态和远程退出协议。
  - 证据：`signal_state.lock` 保护共享 action/pending 与 task mask 交接； `PROC_STOPPED` 使用显式 resume；远程退出发布 `exit_pending`，不再把任意 blocked task 直接改为 READY。
  - 验证：`make check-signal-exit-boundary` 和 `make check-proc-step5-local`。
- [x] 关闭 timeout heap 的引用、取消、过期和容量边界。
  - 证据：heap entry 保存 `(deadline, task, wait_seq)`；cancel/expiry 唯一摘除；容量失败向 syscall 传播；压力测试覆盖 capacity±1 和 stale timeout isolation。
  - 验证：`make check-timeout-ownership-boundary` 和 `make check-proc-step6-local`。
- [x] 建立 SMP runqueue 迁移和持久抢占请求。
  - 证据：`on_rq -> dispatching -> on_cpu` 所有权链；跨队列迁移按 CPU 编号升序锁定；per-CPU `need_resched` 由安全点消费，IPI 只负责通知。
  - 验证：`make check-smp-runqueue-boundary` 和 `make check-proc-step7-local`。
- [x] 从本地 pick 热路径移除全局 `proc_lock`。
  - 证据：`proc_runq_pick_local()` 只持本 CPU runqueue 锁完成 `on_rq -> dispatching`，释放队列锁后调用者才获取 `proc_lock`； `/proc/a20/task_lifetime` 暴露 pick、争用和并行峰值。
  - 验证：`make check-process-lock-split-boundary` 和 `make check-proc-step8-local`。
  - 新测量（`fqwqf/performance-overhaul`，`-smp 8 thread=multi` mm_stress）：`proc_lock` 仍是唯一显著 热点（约 3.3 万次竞争/1600 万自旋），callsite 归因显示竞争集中于**互斥量 park/wake 协议**与 **切换发布路径**；把 pick 拆出只是第一步，完整消除需按等待对象分锁并合并切换路径的两次获取， 见 `docs/roadmap/perf-overhaul.md` §3。
- [x] 压缩 `proc_lock` 的剩余获取点。
  - 证据：`sched()` idle 空队列快速路径（无任务可唤醒 idle，跳过两次全局锁获取）；`proc_switch_complete()` 无待交接任务时原子检查后直接返回；EEVDF enqueue 常见情形 O(1) 尾部追加（新 deadline 晚于队尾时不再遍历）；SIGALRM 到期扫描改为 per-task alarm 最小堆（`proc_set_alarm_expire` 维护，expiry O(k log n)，destroy 时移除条目）；`proc_wake_child_waiters_locked` 用 proc_lock 保护的全局面等待者计数在无人 wait 时 O(1) 返回。阻塞切换的 completion 仍必须持 `proc_lock`：wake 竞争 enqueue 的仲裁是 park 协议正确性部件。
  - 验证：`make smoke-riscv64`、`smoke-sched-stress`、`smoke-proc-stress`、`smoke-futex-stress`、`smoke-abi-linux` 全部 PASS；guest 内 `sleep 1` 的 SIGALRM 正常；`[SMP] 2/2 configured CPUs online` 后干净关机（2026-08 历史记录）。
  - 剩余：`proc_wait4` 的 child 全局扫描已在 `perf/core-modernization` 解决 （per-task children/线程组链表，见 `docs/roadmap/perf-overhaul.md` §7.2，2026-08-25）； 切换路径的两次获取已在此前回合合并。
- [x] 修复低地址用户 `execve` 参数在 identity-mapped 架构上的来源误判。
  - 证据：`proc_exec()` 始终按用户指针复制 `argv/envp`； `proc_stress` 在 `0x02000000` 构造参数数组。
  - 历史验证：双架构 `PROC_STRESS: low-user-argv PASS` 与功能测例全通过（历史平台记录，当前状态需复验）。
- [ ] 将仍依赖单线程执行的 MM 路径改为在 VMA 和页表修改期间持有 `mm->lock`。
  - 当前证据：`kernel/include/mm/vm.h` 仍明确说明部分路径依赖单线程执行或更窄的局部锁；现有 gate 包含 MM smoke/fork-exec race，但不等于所有列举竞争已覆盖。
  - 完成条件：`make check-mm-lock-model` 包含 concurrent mmap、munmap、fault、fork COW 和 exit teardown 的行为测试。
- [x] 为 close/read/write、dup/close_range、rename/unlink/open、symlink loop 和 mount/unmount 增加运行时 VFS 并发压力测试。
  - 源码证据：`user/cmds/stress/vfs_stress.c` 新增四个多进程竞争场景——`concurrent_rw_dup_churn`（双子进程写/读竞争同一文件 + 父进程 dup/close_range churn）、`concurrent_unlink_open_race`（unlink→create→write 循环 vs open/fstat 竞争，ENOENT 为合法竞态结果、其余 errno 即失败）、`concurrent_symlink_resolution`（symlink 拆除重建 vs 解析读取穿透）、`concurrent_mount_umount`（ramfs 同点挂载/卸载竞争 vs stat，容忍 EBUSY/ENOENT 竞态）。这些场景经 `smoke-vfs-stress` 执行：内核在竞争中崩溃、返回错误 errno 或数据不一致即门禁失败——满足"能在竞争回归时失败"的完成条件。
  - 验证：`make smoke-vfs-stress` riscv64 PASS（当前提交）。
- [x] 用 EEVDF 替换 8 级 MLFQ，作为普通任务的统一调度核心。
  - 证据：`kernel/proc/sched.c` 实现加权 vruntime、系统虚拟时间资格门控、虚拟截止时间选择、空闲窃取与时间片旋钮；`/proc/a20/sched_base_slice` 可运行时切换桌面/HPC。当前 nice -20/19 权重为 43020/88，理论份额比约 489:1；运行时分布结论必须引用具体历史样本，不能写成当前测量。
  - 历史验证：`check-doc-test-gates`、双架构 sched/futex/proc 压力和 riscv64 8 核 `sched_stress` smp-runqueue + lock-split PASS（历史平台记录，当前状态需复验）。
  - 设计：`docs/eevdf-scheduler.md`。

## P0：Linux ABI 正确性

- [ ] 在 syscall 组 smoke 和边界测试齐备后，把满足条件的 Linux syscall 区域从 `partial` 提升到 `full`。
  - 当前证据：`kernel/abi/linux/syscall_coverage.md` 仍将 fd I/O、path、process lifecycle、signal、MM、futex、poll、socket 和 timer 标记为 partial；当前没有区域完成 `full` 升级条件。
  - 完成条件：每个升级区域都在覆盖表条目旁列出对应测试。
- [ ] 用符合受支持 Linux 语义的行为替换 scheduler policy 和 affinity 近似实现。
  - 当前证据：`kernel/abi/linux/syscall_coverage.md` 仍将 scheduler 标为 partial，RT/deadline/cgroup/topology 语义有边界。
  - 完成条件：sched policy、priority、affinity 和 cgroup cpuset 行为被 LTP 风格测试覆盖。
- [x] 完成高级 futex 操作和内存顺序边界语义。
  - 当前证据：`kernel/abi/linux/syscall_coverage.md` 将 futex 标记为 partial；全部标准命令已实现（WAIT/WAKE/BITSET/REQUEUE/CMP_REQUEUE/WAKE_OP 及有边界的 PI 变体），但优先级继承提升与完整内存顺序边界仍有缺口。
  - 已落地（2026-08-26）：PI 优先级继承以 EEVDF 权重捐赠实现——等待者 park 期间 owner 的有效权重提升至不低于等待者（`task_t::pi_boost_weight`，`eevdf_weight()` 动态读取），owner 的 vruntime 按等待者速率累积并保持 eligible；UNLOCK_PI 清除捐赠；多锁 owner 近似与链式 pi_state 游走记录为范围外。修复 LOCK_PI 的 OWNER_DIED 重获取缺陷：旧路径 CAS 期望 0 而对 OWNER_DIED 词恒失败形成内核忙等，现保留 OWNER_DIED|WAITERS 位获取（Linux 语义，获取方可察觉不一致状态）。`smoke-futex-stress` 新增 pi-roundtrip / pi-self-deadlock / pi-owner-died 三场景，全部 PASS。
  - 完成条件：basic、requeue、private/shared、timeout、PI 和 robust-list 场景都有覆盖。
- [x] 决定哪些显式 `-ENOSYS` Linux syscall 占位符仍在范围外，哪些应该实现。
  - 证据：`kernel/abi/linux/syscall_table.def` 不再有固定 `-ENOSYS` 占位符；fanotify、acct、keyring、AIO、module、userfaultfd、perf_event_open、arch_prctl 均已实现，仅存的 `-ENOSYS` 是架构/版本正确的 Linux 语义（nfsservctl、map_shadow_stack、riscv_*、arch_prctl 非 x86 fallback）。
  - 完成条件：每个占位符都有记录在案的 owner 决策，见 `kernel/abi/linux/syscall_coverage.md` 的 "Placeholder Resolution Record"。
- [x] 修复 syscall 参数求和/乘法溢出与无界循环。
  - 证据：`sys_sendmsg`/`sys_recvmsg` 的 iov 长度求和增加回绕检查（`SIZE_MAX - total`），`sys_move_pages` 的 `nr_pages * sizeof(int)` 增加乘法溢出检查并把分配失败返回 `-ENOMEM`，`sys_readv`/`sys_writev` 增加 `iovcnt` 负值与 >1024 检查，与 `sys_sendmsg` 的 1024 上限一致。
  - 验证：riscv64/loongarch64 `BRINGUP=1` 构建通过；`smoke-abi-linux` 类 syscall smoke 未受影响（2026-08 历史记录）。

## P0：MM、Page Cache 与文件映射

- [x] 为 `MAP_SHARED` 文件映射增加 dirty-page/writeback owner。
  - 源码证据：`kernel/include/mm/vm.h` 规定 shared file VMA 映射 canonical page-cache page并持 pin，dirty PTE 在 fsync/msync 前同步到 page cache；`user/cmds/stress/mm_stress.c` 覆盖 write/read、fsync、truncate、fork 和 remap 可见性。
  - 完成条件：shared mmap 写入按受支持语义在 read、fsync、truncate、fork 和 remap 路径中可见。
- [x] 加强跨 file read/write、mmap fault、truncation 和 reclaim 的 page cache eviction 与 coherence 测试。
  - 源码证据：`user/cmds/stress/mm_stress.c` 的 shared-file coherence、`shared_file_eviction_pressure()` 和 `shared_file_eviction_with_mmap()` 在超过 cache 容量的压力下检查回读、mapped page 与 pin 回收。
  - 完成条件：page-cache 测试在内存压力下运行，并能捕获 stale data 或 use-after-free 回归。
- [x] 验证 fork、mprotect 和 munmap 下的 huge-page demotion 与 COW 行为。
  - 源码证据：`user/cmds/stress/mm_stress.c` 覆盖 huge-page basic、fork COW、partial munmap demotion 和 mprotect demotion；核心锁模型规定 TLB/refcount 顺序。当前测试不包含 huge-page OOM reclaim，因此该项只表示已覆盖列出的 fork/mprotect/munmap 路径。
  - 完成条件：回归测试覆盖混合 huge/small 映射、write fault 和对 TLB flush 敏感的场景。
- [ ] 修复 shmring 进程组退出后 poweroff 路径的 pfa 空闲链损坏（"corrupted next link"，`kernel/mm/frame.c`）。
  - 复现：`make smoke-native-shmring`——功能基准 NATIVE_SHMRING: PASS 后执行 poweroff 即触发；本工作树与基线 720e16ab0 均复现（2026-08-26 记录）。怀疑方向：共享 VMO 多进程退出时帧归还路径的重复释放或链表破坏；与 IDL 迁移无关（常量值不变、纯编译期挪位）。
  - 已收集证据（同日）：损坏并非简单双重归还——为 frame.c 加装空闲帧位图检测器（fl_push 查重/fl_remove 清位，双重归还即时 panic）后未触发 DOUBLE-FREE，但症状随内核保留区布局移动一页而从"alloc 时 panic"变为"poweroff 后静默挂起"，属布局敏感的潜在破坏。调用链事实：sys_reboot 的 POWER_OFF 分支直接 firmware_shutdown()，不做任何清理；因此损坏实际发生于数据面运行或进程退出期间，poweroff 前最后一次用户态分配只是引爆点。位图检测器作为永久硬化保留（静默损坏→带归属即时 panic）。追加操作历史环形缓冲（最近 512 次 push/remove，corruption/double-free panic 时转储最近 48 条）用于下次 panic 现场取证。症状观察：在内核保留区插入位图（整体后移不足一页）后，同场景从"alloc 时 panic"转为"poweroff 后静默挂起"，提示破坏点与表现形态对布局/时序敏感。
  - QEMU HMP 取证（2026-08-26，挂起现场）：vCPU running、scause=0xf（store page fault）、pc 位于 `__trap_from_kernel+0x4`（`sd ra,8(sp)`）、stval=`0xffffffbb94b14b98`。即 trap 入口用已损坏的 sp 保存寄存器时立即再次 fault，无限重入形成静默空转——坏值非正常内核栈段（应为 0xffffffc0xxxxxxxx）。下一步方向：排查该场景下 task_spawn handle 传递/进程退出路径的内核栈分配逻辑，以及是否存在越界写污染相邻内存（与 pfa 链表损坏可能同源）。
  - 入口硬化实施后的 HMP 取证更新（同日）：guard 生效后症状仍为 poweroff 后静默空转，但采样定位精确化——vCPU 稳定停在 `__trap_from_kernel` 保存序列内（pc=...c356，六次采样五次一致），scause=0xf、stval=`0xffffffc05cf1d158` 位于直映射区内部（>= PAGE_OFFSET，通过下界检查）。结论：坏 sp 并非随机垃圾，而是指向直映射区中已释放/未映射页的"格式合法"指针——与 pfa 空闲链损坏同源（页归还后元数据/内容被复用破坏）。下一步：审计 shmring 场程中内核栈/VMO 页在进程退出时的归还与复用路径。
  - gdbstub 页表取证确认（同日）：挂起时 satp=SV39 有效、sp=0xffffffc00729de70 指向的页 **PTE 无效**（gdb "Cannot access memory"），即内核栈页已被从当前页表 unmap。这是**进程退出顺序（teardown ordering）问题**：栈页释放/unmap 发生在最终 trap 处理完成之前，后续 trap 尝试使用已 unmap 的栈导致无限 fault 循环。修复方向：审查 proc_exit/mm_destroy 路径中内核栈页 unmap 与最后一次 trap 返回的顺序，确保在所有可能的 trap 处理完成之前不解除栈页映射。
  - 排除性结论（同日）：为 vmo_destroy/vmo_resize 加装逐页 pfn_valid 校验（越界即带 index/pfn 精确 panic）后重跑 shmring 门禁——校验零触发、功能 PASS、poweroff 后挂起依旧。VMO pages 数组本身完好；损坏源收敛为 pfa.meta[] 元数据字段（prev/next/order/flags）被直接覆写，需排查所有经 meta_of() 的写入方与相邻内存越界波及的可能。
  - 机理修正（同日，多次采样）：挂起期间 HMP 六次采样 PC 恒定于 `__trap_from_kernel` 内同一条 sd 指令、stval 恒定 `0xffffffc05cf1d158`（对应物理偏移约 1.46GB，超出 1GB RAM 管理上限——越界地址）。恒定的 pc/stval 排除"sp 逐次递减的重入"模型，确认为 **fault → C handler 判定不可处理 → 返回 → 重试同一指令** 的完整往返死循环。下一轮调查方法建议：QEMU gdbstub（-s -S 或运行中 attach）单步跟踪该指令前后状态，确定指令语义与其访问地址的产生路径；或在 handle_page_fault 的不可处理分支加入一次性 dump（scause/sepc/stval/当前任务/PC 所在符号）后 panic，将静默循环转为带现场的显式崩溃。
  - 入口 guard 实测（同日）：guard 就位后症状仍为 poweroff 后静默空转且 [TRAP] 报告未触发——坏 sp 通过了下界检查，即它是"格式合法的直映射区指针"，指向已归还 buddy 的页（页映射/内容已失效）。与 pfa 空闲链损坏同源的假设进一步强化：某结构的内核栈指针在其宿主页被归还后仍被解引用。精确修复需对照 pfa.ranges 与空闲位图校验 sp 对应 pfn 的归属状态，或定位归还后仍持指针的结构。
  - QEMU gdbstub 取证确认（同日）：挂起时 sp=`0xffffffc0700a81b0`，gdb 报 "Cannot access memory"——宿主页确实已释放/未映射；`t6=0xffffffc000000000` 为 guard 常量残留（证明入口检查执行过并通过）；pc 位于 FP 保存区（比 GP 区更深），确认整个寄存器保存路径都在无效栈上空转。ra=`0xffffffc08020f22c`、tp/a0=`0xffffffc0beea2010` 均为有效内核地址，说明损坏仅影响栈页本身而非全局状态。
  - trap.S 机理确认（2026-08-26）：`__trap_from_kernel` 无栈交换（注释 "already on kernel stack"），入口即 `addi sp,sp,-70*8` 后逐寄存器 sd 保存——坏 sp 每次重入继续递减，fault 地址随之漂移，形成不可自愈的死循环。入口处无任何 sp 合法性防护；可选硬化：汇编层对 sp 做范围粗检（不在内核直映射区间即换用应急栈并 panic），把静默空转变成带现场的一次性崩溃。
  - 双界守卫实测（同日）：守卫扩展为双界——PAGE_OFFSET 下界 + pfa 导出的直映射上界（`g_pfa_direct_map_end`，pfa_init 按 RAM ranges 计算，初始化前 UINT64_MAX 即禁用检查，避免早期启动误判）。重跑 smoke-native-shmring：功能 PASS，但本次挂起形态迁移到更早点——`[SD] POWER_OFF entry` 未打印即静默，守卫未触发（竞态缺陷的形态对布局/时序敏感）。历史捕获的越界 sp 现场（1.45–1.75GB 物理偏移）此后会被守卫转为带归属 panic 而非静默空转。proc_exit 审计补充：内核栈为 kmalloc 分配（fork.c/proc.c），于 reap 路径 kfree（task.c），proc_release_exiting_mm 只处理用户 mm——读码未见显式顺序倒置，根因仍未定界。
- [x] 让 OOM reclaim 策略可观察、可测试。
  - 源码证据：`user/cmds/stress/oom_stress.c` 在 cgroup2 `memory.max` 限制下用子进程触发按页耗尽，断言 victim 以 SIGKILL 死亡、`memory.events` 的 `oom_kill`/failcnt 计数推进、幸存父进程仍能分配内存和做文件 I/O；`make smoke-oom-stress` 在 QEMU 中执行该场景。配套修复：`kernel/core/trap.c` 在缺页失败路径先检查 pending fatal signal（cgroup/global OOM kill 刚发出的 SIGKILL），不再把 OOM 受害者误报成 SIGSEGV，与 Linux 对 VM_FAULT_OOM 的 fatal-signal-pending 处理一致。
  - 完成条件：OOM 测试证明 safe kill/reclaim 行为，而不是只记录分配失败日志。

## P1：I/O 进展与网络

- [ ] 在块设备和网络设备能够发出完成信号的位置，用事件驱动 wakeup 替换 scheduler/idle 轮询进展。
  - 证据：`docs/external-dependencies.md` 描述了基于轮询的 lwIP 进展；`kernel/drivers/block/virtio_blk.c` 记录了未来 interrupt wake 路径。
  - 设计：`docs/drivers/guide/lock-order.md`（驱动锁契约）、`docs/net/network-lock-contract.md`（deferred bottom-half 规则）；用户决策：deferred bottom-half / workqueue。
  - 完成条件：块设备和网络进展在正常运行中不再依赖通用 hot-path 轮询。
- [ ] 降低 `g_lwip_lock` 竞争，并为所有 socket 路径记录锁安全入口点。
  - 证据：`kernel/net/lwip_stack.c` 用全局锁串行化 lwIP 核心状态；`kernel/include/core/lock.h` 限制 lwIP 锁下的调用。
  - 设计：`docs/net/network-lock-contract.md`。
  - 完成条件：socket send/recv/connect/listen/accept 测试可并发运行，且没有锁顺序告警或饥饿。
- [ ] 用 board/network 配置管线替换仅适用于 QEMU 的网络地址默认值。
  - 证据：`docs/external-dependencies.md` 说明 `10.0.2.15`、`10.0.2.2` 和 `10.0.2.3` 只是开发默认值。
  - 设计：`docs/net/network-config-design.md`；用户决策：只使用命令行 / 运行时配置，不使用编译期板级默认值。
  - 完成条件：真实开发板或非 QEMU 后端无需硬编码 QEMU 假设即可配置 IP、gateway 和 DNS。
- [x] 扩展现有网络 smoke，使关键语义不依赖可跳过项并覆盖 partial I/O/error path。
  - 证据：`smoke-network-suite` 已存在，RISC-V64 单核运行 `network_suite`；聚合 DNS、TCP/UDP/ICMP loopback、AF_UNIX、AF_ALG 和 timeout，其中 DNS/AF_ALG 可返回 77 跳过。
  - 已落地：新增 `tcp_edge_test` 作为不可跳过场景（partial-io 短读流完整性 / ECONNREFUSED 无监听端口 / EPIPE 对端关闭后写）；内核按 Linux ABI 将"曾建立连接的流套接字在端点消亡后的写"映射为 EPIPE（此前经无连接兜底路径错误地返回 ECONNREFUSED），覆盖本机快速路径与远程 dead-pcb 两种形态；新增 `smoke-network-suite-aarch64` 其他架构运行入口并修复两个预先存在的 aarch64 构建缺陷（`g_vinput_pdev` 声明守卫、native 用户态缺 `-mno-outline-atomics`）。
  - 门禁：`make smoke-network-suite`（riscv64）与 `make smoke-network-suite-aarch64` 均 PASS（7 passed, 1 skipped，alg 合法跳过）。

## P1：VFS 与文件系统语义

- [ ] 收紧 path resolution、symlink、permission、mount 和文件系统特定的 Linux 边界语义。
  - 证据：`kernel/abi/linux/syscall_coverage.md` 将 path 和 metadata 标记为 partial，并要求清理。
  - 设计：`docs/fs/vfs-edge-semantics.md`、`docs/fs/fs-consistency-model.md`。`RESOLVE_CACHED` 已实现（dentry 未命中返回 `-EAGAIN`，`0x20` 与 Linux 一致），A20OS 自定义 `NO_TRAILING` flag 已移除、不再与 Linux 冲突；`RESOLVE_NO_MAGICLINKS` 已实现（procfs fd-symlink 带 `VNODE_MAGICLINK` 标记，穿越返回 `-ELOOP`，普通 symlink 不受影响），由 `smoke-vfs-edge` 覆盖。
  - 完成条件：openat、renameat2、link/symlink、chmod/chown、statx、mount、umount 和 chroot 都有聚焦测试（`user/cmds/stress/vfs_stress.c` / `vfs_edge.c`），同时覆盖 openat2、xattr 和文件系统特定边界测试。
- [x] 将大型 VFS 实现重构为更小的 ownership、path、mount、fd 和 syscall-facing 单元。
  - 源码证据：path resolution、path、mount、file/vnode、dcache 和 stat/permission 已拆到 `kernel/fs/vfs/*.c`，并由 `kernel/include/fs/vfs/*.h` 提供窄接口；`kernel/fs/vfs.c` 仍保留 open/close、初始化及兼容入口，后续还可继续缩小。
  - 完成条件：每个单元都有窄 header 契约和子系统特定测试。
- [ ] 尽可能从通用 VFS 路径中移除硬编码运行时文件系统初始化。
  - 证据：`kernel/fs/vfs.c` 在 VFS bringup 期间初始化默认虚拟文件和类似环境的内容。
  - 设计：`docs/fs/fs-consistency-model.md`（ramfs / rootfs 一致性模型）；用户决策：构建期 rootfs overlay / initramfs 风格用户态镜像构造。
  - 完成条件：策略文件迁移到 init/userland image 构造，或迁移到声明式启动文件系统 manifest。
- [x] 为 FAT32、ext4、ramfs、devfs、procfs、sysfs、pipe 和 anonfd 操作定义清晰的一致性模型。
  - 文档证据：`docs/fs/fs-consistency-model.md` 逐后端记录读写、namespace、缓存、持久化和不支持操作边界；这表示模型文档已建立，不表示 Linux path/metadata ABI 已达到 full。
  - 设计：`docs/fs/fs-consistency-model.md`；每后端 unsupported-op errno 矩阵不属于 P1 范围。
  - 完成条件：后端能力差异记录在 `docs/fs/fs-consistency-model.md`；P1 不要求专用 smoke 门禁。

## P1：Native ABI 完成度与可维护性

- [x] 将过大的 Native phase-2 syscall 实现拆分为子系统所有的文件。
  - 源码证据：Native syscall 已拆到 `sys_native_{handle,mm,ipc,security,debug,device,fs,net,sync,system,task,time,ext}.c` 等子系统文件；`sys_phase2.c` 只保留少量尚未迁出的兼容入口。
  - 完成条件：Native syscall 文件映射子系统边界，每个文件只拥有窄 syscall 范围。
- [x] 完成 Native debug 语义，或明确缩小其范围。
  - 证据：`kernel/abi/native/sys_native_debug.c` 提供完整停止/恢复语义（attach/traceme/wait/event/resume/detach/read/write/read_regs/write_regs/kill），与 Linux ABI ptrace 共享同一 `proc_debug_*` 状态机；watchpoint 明确不在范围。
  - 完成条件：debug handle 行为要么完整实现并测试，要么在 Native ABI 文档中记录为有意受限。
- [x] 完成 handle transfer、partial-delivery、temporal-rights 和 label consistency 测试。
  - 证据：`kernel/abi/native/handle_table.h` 和 `kernel/abi/native/handle_table.c` 引用了 partial-delivery 与 capability consistency 门禁。
  - 完成条件：Native IPC 测试覆盖成功 transfer、失败 transfer、被撤销权限、过期权限和 label denial。
- [x] 让 Native ABI 文档与活跃用户态运行时实现重新对齐。
  - 证据：`docs/native-abi/00-overview.md` 描述了已完成的 Native ABI 工作；活跃用户态也大量依赖 Linux ABI musl 构建。
  - 设计：`docs/native-abi/00-overview.md`、`docs/native-abi/08-runtime-status.md`、`user/archive/README.md`；用户决策：将 `liba20c` 更新为使用版本化 ABI 结构体。
  - 完成条件：文档区分活跃 Linux-musl 用户态、`liba20rt`、`liba20c`、已归档 A20 syscall bridge 代码和未来 Native POSIX shim 工作；`liba20c` 使用版本化 ABI 结构体，内核校验 `size`/`version`。

## P1：驱动与设备模型

- [x] 用动态大小或具备容量检查且能报告结构化错误的 registry 替换固定大小 driver/device/bus registry。
  - 源码证据：`kernel/drivers/core/driver_core.c` 的 driver/device/bus registry 从初始容量开始，在锁保护下用 `krealloc` 扩容；扩容失败记录 capacity-exhausted 错误并返回失败，不再静默丢失注册项。
  - 当前验证边界：源码已满足动态扩容与结构化失败要求，但未找到专用的 registry exhaustion 运行测试；该缺口归入 P2 行为门禁扩展。
- [x] 在把驱动模型视为通用模型前，增加 hotplug 和 remove-path 生命周期测试。
  - 源码证据：`kernel/drivers/core/driver_lifecycle_test.c`（CONFIG_DRIVER_LIFECYCLE_TEST 门控，读 /proc/a20/driver_lifecycle 或启动时自动运行）覆盖全部完成条件——bind（成功 probe 绑定 + class_dev 发布 online）、probe failure（强制失败断言无半绑定状态且 probe_count==1）、remove（driver_unregister 与 device_unregister 两路径均断言回调触发与 DEV_STATE_REMOVED）、re-probe（re-register 自动重探 probe_count==2 + 显式重探已绑设备返 -EBUSY）、资源清理（stale class_device 访问返 -ENODEV、out 路径保证注册表必然清理）；另覆盖重复注册 -EEXIST×3 与 bus/driver 双层 match 拒绝。
  - 门禁：smoke-driver-lifecycle riscv64 PASS；顺带修复该目标构建命令缺失 USER_BUILD_DESKTOP=0 导致连带编译 GUI 桌面组件的问题——本测试为纯内核侧，无需桌面组件。
- [x] 将设备特定锁顺序移动到驱动文档中，放在每个私有锁旁边。
  - 源码证据：以下驱动的私有锁声明旁均已添加 LOCK_ORDER 注释记录各自的保护范围和嵌套规则——virtio-blk（4 处）、virtio-net（8 处）、UART（11 处）、PTY（完整 per-pair + alloc lock 体系）、loop（9 处）、SDIO/dw_sdio、ls2k_gmac、starfive_gmac、e1000（per-NIC lock 与 g_lwip_lock 无嵌套）。
  - 设计：`docs/drivers/guide/lock-order.md`；用户决策：面向用户的 `/proc/a20/driver_lifecycle` 触发器。
  - 完成条件达成：virtio-blk、virtio-net、UART、PTY、loop、SDIO 和平台 NIC 都记录了各自私有锁规则。

## P2：测试门禁与工具

- [x] 将静态 `rg` 风格架构门禁转换为行为测试，只要该行为能在 QEMU 下执行。
  - 证据：`check-blocking-point-boundary`、`check-signal-exit-boundary`、`check-timeout-ownership-boundary`、`check-smp-runqueue-boundary`、`check-process-lock-split-boundary` 现在依赖对应 QEMU runtime smoke（`smoke-proc-stress`/`smoke-futex-stress`/`smoke-sched-stress`/`smoke-timeout-test`），在运行时日志 grep 标记而非源码；`smoke-riscv64`/`smoke-loongarch64`/`smoke-aarch64`/`smoke-x86_64` 不再把 watchdog timeout 当 PASS，要求 `part ok` 与正常 poweroff；新增 `smoke-pty-stress` 与 `smoke-timeout-test` 覆盖原本从未运行的压力程序。
  - 验证：`make smoke-riscv64`、`make smoke-loongarch64`、`make smoke-sched-stress`、`make smoke-futex-stress`、`make smoke-proc-stress`、`make smoke-timeout-test`、`make smoke-pty-stress`、`make check-timeout-ownership-boundary` 在 2026-08-16 于 riscv64 全部 PASS（历史记录，当前状态需复验）。
  - 后续：`check-arch-boundary` 已依赖 `smoke-arch-mmu-matrix`（8 组合 MMU/NOMMU runtime 矩阵）；`check-concurrency-foundation` 已依赖 `smoke-smp-bringup`（riscv64 `-smp 2` 真实双核启动并干净关机，`[SMP] 2/2 configured CPUs online`）。剩余：`pty_stress`/`timeout_test` 的多架构入口未建立。
- [ ] 在声明更广兼容性前，为每个 Linux ABI 覆盖区域增加 LTP 风格分组 smoke 测试。
  - 证据：`kernel/abi/linux/syscall_coverage.md` 说明每个 syscall 组在升级级别前都需要 smoke 测试。
  - 完成条件：覆盖表生成包含测试目标名称和 last-known status。
- [ ] 将现有 Native ABI 测试聚合为多架构 CI-like 运行矩阵。
  - 当前证据：源码已有 16 个 `test_native_*.c`，以及 liba20c/mlibc、service、personality smoke；不再只是 minimal/libc。问题是目标分散，且多数 QEMU smoke 固定为 RISC-V64。
  - 完成条件：Native handle、VMO/VMAR、channel、event queue、timer、task、debug 和 rights 测试进入可枚举的多架构 CI-like 矩阵。
- [ ] 增加 memory pressure、fork/exec churn、fd churn、filesystem churn、network churn 和 process reaping 压力测试。
  - 证据：当前 smoke 目标证明基本操作，但不能证明长时间稳定性或竞争行为。
  - 完成条件：stress 目标带有有界 timeout，并在失败时捕获内核日志。

## P2：仓库卫生与依赖边界

- [x] 从活跃源码目录中移除或隔离 patch artifact 文件。
  - 当前证据：`kernel/` 下已没有 `*.orig`/`*.rej`；历史上的 `fork.c.orig`、`fork.c.rej` 和 `sys_futex.c.orig` 等 artifact 已退出活跃树。
  - 完成条件：活跃源码目录只包含构建输入、文档或有意跟踪的 fixture。
- [x] 内核编译警告清零并启用 `-Werror`。
  - 证据：根 `Makefile` 的 `CFLAGS` 默认带 `-Werror`（`KERNEL_WERROR=0` 逃生口）；riscv64/loongarch64 的 `linux` 与 `both` ABI、bringup/dev/SMP2 配置全部零警告。顺带修复：`io_pgetevents` 读取 `arg[6]` 越界（64 位 ABI 无第 7 参）、COW fault 全局计数不在成功分支内、`a20_prepare_start_info` 的 uint32 handle 错误检测恒假（失败时静默写入 0xFFFFFFFF）。架构门禁列表合并为 `SUPPORTED_HOSTED_ARCHES` 单一真源；STM32 产物路径由 `BUILD_VARIANT` 派生，flash/QEMU 目标经嵌套 make 保持同配置。
  - 验证：`make -B ARCH=riscv64/loongarch64 ABI=linux/both BRINGUP=1 kernel-only` 零警告；`smoke-native-handle`、`smoke-native-contract` PASS（2026-08 历史记录）。
  - 剩余：aarch64/x86_64/arm32/riscv32/ppc64le/armv7m/loongarch32 的警告状态未在无工具链主机上复核，首次构建如遇残留警告需 `KERNEL_WERROR=0` 过渡。
- [ ] 除非测试明确是集成测试，否则不要把 vendored code 纳入第一方质量声明和测试。
  - 证据：`docs/external-dependencies.md` 将 lwIP、musl、sbase、mksh、TLSe 和 wget 的角色与 A20 集成工作区分开。
  - 完成条件：TODO 和状态文档一致地把 A20 的工作归功于集成，而不是上游 TCP/IP、libc、shell 或 coreutils 实现。
- [x] 增加外部依赖升级 checklist 目标，自动运行相关 smoke 组。
  - 源码证据：`tools/targets-build.mk` 新增 `check-upgrade-userland-smokes` 聚合目标（smoke-abi-linux + smoke-mlibc + smoke-mlibc-sbase + smoke-mlibc-mksh，即 syscall/shell/coreutils 三组）并注册进 check 目标帮助列表；`docs/external-dependencies.md` 的 EXTERNAL_USERLAND_UPGRADE_CHECKLIST 条目现指向具体目标名。修改 musl/sbase/mksh 后一条命令即可运行全部相关门禁组；lwIP 见网络节、TLSe/wget 见各自集成说明。
  - 验证：`make -n check-upgrade-userland-smokes` 依赖解析正确（当前提交）。
- [x] 记录哪些归档用户态代码只是历史参考，哪些预计会恢复。
  - 源码证据：`user/archive/README.md` 完整列出各子目录的历史定位（旧 native coreutils/libc/shell、musl 桥接层、sysroot 脚本、早期实验件），明确声明不参与当前构建、部分引用路径已失效、活跃替代位于 `user/liba20rt/`、`user/liba20c/` 与 `kernel/abi/linux/`；并新增恢复预期说明——当前无计划原样恢复任何组件，重启即重设计；将来移回活跃树必须同时给出 owner 与覆盖测试入口。
  - 完成条件达成：归档路径已通过 README 从活跃状态声明中排除。

## 验证环境说明

- 文档不再固化某一台 host 的工具缺失状态。工具链和 QEMU 可用性由对应 build/smoke 目标在运行时报告。
- 本文按 2026-08 源码核对。文中的验证记录均为历史记录：PASS 只表示它在标注的日期与配置下通过，引用为当前结论前必须在当前提交上重新运行。
- Proc/Sched 的当前累计静态门禁是 `make check-doc-test-gates`；双架构 debug/release、1 核/8 核运行矩阵是 `make check-proc-step8-local`。
- 需要完整双架构运行矩阵时运行 `make check-proc-step8`，它聚合 RISC-V64 与 LoongArch64 的 debug/release、单核/八核压力矩阵。
- 项目 Python 命令统一通过 conda 环境 `a20os`；长跑基准入口负责记录 QEMU 命令、镜像哈希、退出状态、timeout 与 guest CPU 状态。
- NOMMU 支持集合由构建入口和 `smoke-arch-mmu-matrix` 验证，不以本文中的 历史成功列表代替当前运行结果。
