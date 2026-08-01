# A20OS Native ABI 运行时状态

本文档记录 A20OS Native ABI 当前用户态运行时的真实状态、已知偏差和后续路线图。它是对 [00-overview.md](00-overview.md) 中实现状态的补充。

## 当前运行时状态

| 组件 | 路径 | 状态 | 说明 |
|------|------|------|------|
| Linux ABI 兼容层 | `kernel/abi/linux/` | 活跃 | 当前主用户态运行时接口。系统启动后实际运行的用户态程序基于 Linux ABI。 |
| Native ABI 内核入口 | `kernel/abi/native/` | 已实现 | 93 个 syscall 入口已完成（`syscall_table.def` 93 条），覆盖 core、handle、task、memory、path、ipc、net、time、security、debug、system info、sync 等分区。 |
| Typed channel | `kernel/ipc/a20_channel.c` | 已接入 | `channel_create` 接受 `a20_channel_type_t` 类型签名（每端点一份拷贝），send/recv 路径强制执行 handle 类型 bitmask 与 `max_data_size`/`max_handles` 上限，违例返回 `TYPE_MISMATCH`。 |
| 时态能力 | `kernel/abi/native/handle_table.c` | 已接入 | `handle_control` 提供 `SET_TEMPORAL`/`GET_TEMPORAL`/`SET_LABEL` 入口（仅可增强不可减弱）；sweeper 以 deadline 驱动周期运行（约 100ms），`AUTO_CLOSE` 过期自动回收；channel 传递保留时态约束与安全标签（不可刷新）。 |
| 阻塞 IPC | `kernel/ipc/a20_channel.c`、`kernel/ipc/a20_event.c` | 已实现 | `channel_send`（队列满）/`channel_recv`（队列空）/`event_wait`（无事件）默认阻塞，`A20_MSG_NONBLOCK`/`timeout_ns=0` 为非阻塞；`event_wait` 支持相对超时与多事件返回，基于 tokenized Park/Wake（见 `docs/process-scheduler.md`）。 |
| 对象级联释放 | `kernel/ipc/a20_object.c`、`kernel/abi/native/handle_table.c` | 已实现 | `handle_close`/`handle_replace`/进程退出/sweeper 统一按类型释放对象：vfile fd、channel 端点（置 `peer_closed` 并唤醒对端）、event queue、VMO、timer 槽、namespace。channel recv 采用 reserve-then-dequeue，HT 满时返回 `NO_SPACE` 且消息留队（无部分投递）。 |
| liba20rt Native SDK | `user/liba20rt/` | 活跃 | 当前活跃的原生 SDK，提供 `a20_syscall.h` syscall wrapper、多架构 crt0 启动汇编、`a20_types.h` ABI 类型定义和高层 handle I/O 头文件。channel wrapper 已修复 `version` 字段并新增 typed create 与 nonblock 变体；`a20_handle.h` 新增时态/标签控制 wrapper。 |
| liba20c 最小 C 库 | `user/liba20c/` | 活跃 | 已实现 malloc、stdio、unistd、string、time、errno、fdtable 等 ISO C 子集。`malloc.c`、`unistd.c`、`stdio.c`、`bare_alloc.c` 已改为使用带 `size` 和 `version` 的版本化 ABI 结构体调用 syscall。 |
| 原生测试 | `user/tests/test_native_*.c`、`user/liba20c` 内部测试 | 少量存在 | `test_native_handle.c` 现覆盖 rights 降级、transfer、typed channel 强制、channel 阻塞收/发、event_wait 阻塞与 timer 事件、时态 op-count、时态过期 AUTO_CLOSE；另有 `test_native_futex.c`、`test_native_hello.c`、`test_native_minimal.c`、`test_liba20c.c`。不是历史上宣称的 4 套 118 cases host-mode 测试套件。 |
| musl 移植目录 | `user/musl-port/` | 不存在 | 该目录未在当前仓库中创建。相关历史材料已移至 `user/archive/`。 |
| 历史参考 | `user/archive/` | 不参与构建 | 包含旧版 musl 桥接、`a20coreutils`、`build_sysroot.sh`、`arch/a20/` 适配头等。这些代码仅供历史参考，路径和内容已过时，不进入当前构建。 |
| Debug handle | `kernel/abi/native/` 0x0900 分区 | 有意受限 | 仅支持创建 debug handle、读取寄存器快照和映射目标内存。stop/resume/watchpoint、ptrace 级语义等尚未实现，也不会在需求明确前扩展。 |

## 关键偏差说明

### 1. liba20c 已迁移到版本化 ABI 结构体

`liba20c` 的 `malloc.c`、`unistd.c`、`stdio.c`、`bare_alloc.c` 已完成迁移：不再使用裸 `uint64_t args[N]` 数组，而是构造带 `.size` 和 `.version` 的版本化结构体（如 `a20_vm_alloc_args_t`、`a20_path_open_args_t`、`a20_io_args_t`）后调用 syscall。

示例（迁移后）：

```c
struct a20_vm_alloc_args args = {
    .size = sizeof(args),
    .version = 1,
    .length = req_size,
    .prot = A20_PROT_READ | A20_PROT_WRITE,
};
int64_t r = a20_vm_alloc(&args);
```

迁移后，`smoke-native-libc` 目标已跑通，裸参数数组调用已不存在于 `user/liba20c/*.c`。

### 2. liba20rt 类型头与内核类型头已对齐

`user/liba20rt/a20_types.h` 与 `kernel/include/abi/native/types.h` 的结构体布局已逐字段对齐（本节为此前的偏差记录，现已修复）。channel wrapper 此前将 `version` 置 0 导致 `A20_VALIDATE_AND_COPY` 拒绝，已修正为 1；`event_wait` 已切换为与 05-ipc.md §3.4 一致的结构化多事件形式。内核侧结构体验证现遵循 01-types.md §2 的演进规则（拒绝短 version-1 结构、长结构体截断、version 0 与超版本拒绝）。

### 3. archive 目录不参与构建

`user/archive/build_sysroot.sh` 引用了 `user/musl-port/`、`user/archive/src/...` 等路径，其中一些在当前仓库中已不存在。不要直接运行该脚本。如果未来需要重新启动完整 musl 移植，应以 `user/archive/` 为参考，而不是直接复用。

### 4. 测试覆盖有限

当前原生测试有 `user/tests/test_native_handle.c`（12 组用例，含 typed channel/阻塞 IPC/时态能力）、`test_native_futex.c`、`test_native_hello.c`、`test_native_minimal.c`、`test_liba20c.c` 和 `user/liba20c` 内部的小规模示例。历史上文档中提到的 4 套 118 cases host-mode 测试套件目前不存在，需要在路线图阶段补齐。

### 5. Sync (0x0B00) 分区与 thread_create 修复（已完成）

- 新增 `futex_wait`/`futex_wake` 两个 native syscall（`kernel/abi/native/sys_native_sync.c`），复用内核 futex 核心。futex 是地址型同步原语而非内核对象，不分配 handle（types.md §22）。
- 修复了 `sys_a20_thread_create` 的三个缺陷：入口地址曾错误写入返回值寄存器而非 PC；`CLONE_THREAD` 标志值曾误用 `CLONE_PARENT`；trap frame 曾在 `proc_make_ready` 之后才被修改（竞态）。现在通过 `proc_create_thread()`（`kernel/proc/fork.c`）在发布前完成入口/参数/TLS 设置。
- Handle table 改为引用计数并在 `CLONE_THREAD` 线程组内共享（进程本地语义），`task_exit` 改为 `proc_exit_group` 语义并只释放本线程引用。
- `abi_mode` 现在在 clone 时继承，native 线程的 syscall 会正确分发到 native 表。
- 冒烟测试：`user/tests/test_native_futex.c`（`make smoke-native-futex`）覆盖值不匹配立即返回、超时、跨线程唤醒三条路径。

### 5a. mlibc 移植（sysdeps/a20，Phase 1–2 已完成）

`user/external/mlibc` 以 vendor 方式引入 [managarm/mlibc](https://github.com/managarm/mlibc)，新增 `sysdeps/a20/` 移植层（约 1800 行），严格遵守 native ABI 设计取舍：fd↔handle 映射在 libc 内完成；无 fork/execve（ENOSYS）；信号为**检查点式模拟**（`sigaction` 记录 handler，`kill` 经 `task_kill` 记录并在阻塞等待返回时于显式检查点投递，见 §5c）；同步原语走 native futex；线程经 `thread_create` + TCB 蹦床；静态链接（`user/mlibc/a20-mlibc.ld` 提供 init_array 边界、PT_TLS phdr 与 `PT_A20_START_INFO` 标记）。

Phase 2 新增：

- **posix_spawn / waitpid**：直接走 `task_spawn`（参数 v2 追加 stdio 继承字段，ABI 追加式演进），不经过 fork/exec；waitpid 由 libc 侧 pid↔task handle 注册表支撑，WNOHANG 经 `handle_poll` 的 `A20_EVENT_EXITED`。
- **pipe**：`sys_pipe` 由 channel 承载，libc 读端做数据报到字节流的缓冲；写端对端关闭映射 EPIPE，读端映射 EOF。
- **poll**：新增 native `handle_poll`（0x010C）非阻塞就绪查询（复用 `vfs_poll_events`），`sys_poll` 用电平轮询 + 睡眠退避（就绪唤醒粒度 10ms）。
- **socket**：socket/bind/connect/listen/accept/socketpair/sendmsg/recvmsg/shutdown/getname 全部接入 net_* 封装。
- **内核配套**：`path_open` 对常规文件授予 EXEC right；`task_spawn` 向子进程 start_info 安装 root/cwd/stdio；`handle_poll` 支持 vfile 系（vfs_poll_events）、channel（消息计数）、task/thread（ZOMBIE）。

构建与验证：`make mlibc-sysroot`（meson+ninja 构建静态 libc.a），`make smoke-mlibc`（QEMU 冒烟：stdio/malloc/文件 I/O/4 线程 mutex/pipe/poll/socketpair/posix_spawn+waitpid，测试程序 `user/tests/test_mlibc_hello.c` + `test_mlibc_child.c`）。

已知限制：fork/execve（ENOSYS，设计取舍）、信号（检查点式模拟已实现，但跨进程 kill 需 pid→handle 注册表、默认动作 SIG_DFL 未退出进程）、动态链接（评估见 §8a）、task_spawn 的非 stdio handle 继承（仅 fd 0/1/2）、多架构交叉文件（仅 riscv64 验证过）。工具链注意：mlibc 需要 glibc LP64 fast 类型与 `_GNU_SOURCE`，elf 工具链用 `-D` 宏补齐（`ci/a20-riscv64.cross-file`、`Makefile` 的 `MLIBC_FAST_TYPE_FLAGS`）。

### 8a. 动态链接工作量评估

Linux ABI 侧已有完整 PT_INTERP 加载，mlibc 的 rtld 现成，native 侧要做的是**封装层**而非新机制：

| 工作项 | 内容 | 预估 |
|--------|------|------|
| 内核：native 二进制 PT_INTERP | native exec 路径复用 interp 加载 | 小（~100 行） |
| 启动协议：auxv 合成 | ld.so 需要 AT_PHDR/AT_ENTRY/AT_BASE/AT_RANDOM；native start_info 无 auxv，需在 `elf_setup_stack_a20` 附加 auxv 区或在 rtld-a20 crt0 合成 | 小-中 |
| 文件映射窗口 | mlibc rtld 需要 `sys_vm_map` 文件回映射：native `vm_map` 入口存在，需验证 demand fault 对文件 VMA 的填充 | 中（内核 MM 联调是主要风险） |
| mlibc 共享库构建 | `-Ddefault_library=both`，ld.so 与 `libc.so` 链接脚本（PT_DYNAMIC、GOT/PLT） | 小 |
| TLS 动态模型 / dlopen | rtld 全部现成，sysdeps 只需 vm_map/vm_protect/文件读取 | 小-中 |
| 总计 | | **约 1–2 周**，70% 是内核 MM/ELF 联调而非新代码 |

设计注意：07-startup.md 目前声明"native 程序不需要 PT_INTERP"。启用动态链接需要修订该条款——建议保持"系统组件静态链接、应用可选动态链接"的分层。

### 6. 空转机制接入与缺失实现补齐（已完成）

- **Typed channel**：`channel_create` 此前硬编码 `NULL` 类型签名，检查函数不可达。现创建路径复制用户类型签名到两个端点，send 强制 `send_handle_types`/`max_data_size`/`max_handles`，recv 强制 `recv_handle_types`。
- **时态能力入口**：此前所有 install 路径时态字段恒为 0、无设置入口，sweeper 无调用者。现 `handle_control(op=SET_TEMPORAL/GET_TEMPORAL/SET_LABEL)` 提供入口，语义为仅可增强（non-refreshability）；sweeper 以 `sched_note_timer_deadline` 驱动按约 100ms 节奏运行，AUTO_CLOSE 真正释放对象。
- **阻塞语义**：channel send/recv 与 event_wait 此前一律返回 `WOULD_BLOCK`。现默认阻塞（tokenized Park/Wake），`A20_MSG_NONBLOCK` 或 `timeout_ns=0` 退回非阻塞；`event_wait` 支持超时（`A20_TIMEOUT_INFINITE` 无限）与最多 64 个事件批量返回。
- **级联释放**：`handle_close` 此前只关闭 vfile 类对象，channel/eventq/VMO/timer/namespace 全部泄漏且对端永远收不到 `peer_closed`。现按类型统一释放，并在对象销毁时清理 event watch（`a20_eventq_on_object_destroy` 已接线到 channel、event queue、VMO、timer、namespace、task 与 vfile fd 键）。
- **channel recv 原子性**：实现 reserve/abort/commit 槽位预留，接收方 HT 满时返回 `NO_SPACE`、消息留队，符合 05-ipc.md §2.6 的"不做部分投递"决策。接收 handle 继承发送方的时态约束与安全标签（此前被清零）。
- **vm_map source**：`vm_map` 此前忽略 `source` 恒创建匿名 VMO。现支持 `MEMORY` handle 直接共享映射（`prot_eff = prot_req ∩ prot_handle`）和 `FILE`/`DEVICE` 的按需分页映射（demand-paged，经核心 page cache，见下文 §7）。
- **事件常量**：`A20_EVENT_*` 事件位定义补齐（01-types.md），timer → `EXPIRED`、task 退出 → `EXITED`、channel → `MESSAGE_READY`/`PEER_CLOSED`/`CLOSED`。task/timer 的事件键统一为 handle entry 的 object 值（pid/slot+1），修复 watch 永不触发的问题。
- **timer 对象**：handle entry object 由 slot 改为 slot+1（slot 0 此前是 NULL 指针、handle 不可用）；timer 槽改为引用计数，`closing` 标记阻断 slot ABA，set/cancel/tick 均在 `g_a20_timers_lock` 下读写，tick 触发时持临时引用完成 notify。
- **对象并发访问**：新增 `a20_handle_lookup_ref_internal()`，在 HT 锁内同步取得对象引用；channel/event/VMO/vfile/socket/namespace 等长期访问路径均已切换，close/sweeper 不再能在 syscall 持有裸指针期间释放对象。
- **用户指针边界**：channel send/recv、native net sendmsg/recvmsg、xattr、`handle_close_many`、`vm_remap` 与 debug memory map 均改为内核 bounce buffer + `copy_from_user`/`copy_to_user`，不再将用户指针交给核心子系统直接解引用。
- **task_spawn 发布协议**：`proc_alloc_user_image(..., defer_ready=1)` 支持延迟就绪；子任务的 `abi_mode`、handle table、start info 栈和 trap SP 全部初始化完成后，父任务才调用 `proc_make_ready()`。
- **channel 并发关闭**：端点最终释放在 `g_ch_lock` 下同时持有濒死端点锁；阻塞发送在等待期间持有 peer 引用，修复对端 close 与 enqueue/wait queue 并发导致的 UAF。
- **ELF 映像清理/栈页**：新增 `elf_load_info_discard()` 回收 spawn 失败路径的 VMA/pgdir/NOMMU 映像；初始用户栈页与栈增长页在映射前清零，避免跨进程页内容泄漏。

### 7. 仍存在的差距（未实现）

> 2026-08 更新：VMO 已迁入核心 MM 层（`kernel/mm/vmo.c`、`kernel/include/mm/vmo.h`），
> VMAR 改为核心 `mm_mmap_vmo`/`mm_munmap`/`mm_mprotect` 的薄包装（`kernel/abi/native/vmar.c`），
> 因此核心 fault 路径不再依赖任何 ABI 头文件，两套 ABI 也不互相包装依赖。

- file/socket/pipe 的 `READABLE`/`WRITABLE`/`ERROR`/`CONNECTION` 事件源尚未接入 VFS/网络栈（event queue 目前只对 channel、timer、task 退出产生事件）。
- `event_watch_fs` 仍是目录级 watch 的壳，不支持路径过滤、不产生 FS 事件。
- VMO 映射已是**按需调页**：`vmar_map` 只建立 `VM_VMO` VMA，页面在首次 fault 时经核心 `handle_demand_fault_locked` 的 VMO 分支按 `vmo_get_page()` 物化；VMO 帧由 VMO 自身持有（类比 page-cache 帧），fork 时父子共享同一批 canonical 帧而非 COW。文件映射走核心 `mm_mmap_file`，经 page cache 需求填充。
- 仍缺：VMAR 不是层级模型；`vm_share` 尚未提供"按地址区间反查 VMO 并导出"的结构化 `a20_vm_share_args_t` 形式。VMO 页分配已接入 cgroup 记账（fault 时对首个触页任务记账、destroy 时返还；`vmo_get_page_charged`）。
- 性能未实测（研究文档 05 的 G1–G7 阈值仍待验证）；静态能力流分析工具未实现。

## 路线图

### 近期：修复 liba20c ABI 结构体使用（已完成）

- [x] 将 `liba20c` 中所有裸参数数组调用改为带 `size` 和 `version` 的版本化 ABI 结构体。
- [x] 统一使用 `a20_types.h` 中定义的 `a20_vm_alloc_args_t`、`a20_path_open_args_t`、`a20_io_args_t` 等结构体。
- [x] 在修改过程中对照 `kernel/include/abi/native/types.h`，同步修正 `liba20rt/a20_types.h` 中的布局偏差。
- [x] 完成后在目标架构上跑通现有 liba20c 示例测试（`smoke-native-libc`）。

### 中期：补齐原生测试与示例

- 为 `liba20rt` 添加 handle I/O、channel、event queue 的基础测试。
- 为 `liba20c` 添加 malloc、stdio、unistd 的回归测试。
- 建立 `smoke-native-libc` 或类似的最小 smoke 目标。

### 远期：按需扩展 Native 用户态生态

- 明确需求后，决定是否重新启动完整 musl 移植。
- 若重启，应基于 `user/archive/` 的参考代码重新设计，而不是直接复用旧路径。
- Debug handle 的 stop/resume/watchpoint 能力也只在有明确调试需求时扩展。

## 与用户决策的对应关系

- 用户已确认：Linux ABI 继续作为主用户态接口，`abi/native` 保持为辅。
- 用户已确认：`liba20c` 将更新为使用版本化 ABI 结构体。该代码变更不在本文档更新范围内，仅作为路线图记录。
- 用户已确认：`user/archive/` 作为历史参考保留，不参与当前构建。
- 用户已确认：Debug handle 保持 intentionally limited，不盲目扩展为完整 ptrace。
