# CORE 缺口

对应 V1-CORE。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### CORE-07 arm32 的 FP 上下文

- 现状：arm32 现在**能编过也能启动**。内核镜像（容器内 `make ARCH=arm32 ABI=linux BRINGUP=1 dev-build`，QEMU virt/cortex-a15/1G）跑到 `part ok` 并干净关机（rc=0）；用户态镜像（`ABI=both BRINGUP=0`）也能启动到 shell，`echo` 等内建命令可用，但 fork 出去的命令（`/bin/echo`、`poweroff`）返回后 mksh 空指针崩溃（见 [ABI-11](abi-ipc.md)），所以用户态镜像还不能干净关机。用户态 FP 上下文仍未保存：`trap_context_t` 只有 84 字节 GPR 部分，没有 `d0-d15`/FPSCR，两个任务共享同一份物理 FP 寄存器文件。取证：`user/cmds/core/fp_touch.c` 证明 EL0 的 VFP 本身可用（`FP_TOUCH: PASS`，CPACR/FPEXC 在 `arch/arm32/boot/entry.S` 已开）；`fork_fpu_probe.c` 的 arm32 分支（父进程装 sentinel → 让子进程装另一组 → 父进程读回自己的）已写好，用自旋代替 `usleep`，因为 **arm32 的 `sleep`/`nanosleep` 根本不唤醒**。在 trap 帧尾部加 VFP 保存/恢复的尝试（TF_SIZE 84→224、保存/恢复 + 帧拷贝循环）会让第一次用户态 syscall 之后挂死，未定位，改动已撤回。
- 缺口：arm32 上 FP 上下文切换没有实现也没有用例；arm32 的等待/超时路径本身有缺陷（`sleep 1` 永不返回）；fork 出去的命令返回后 shell 崩溃（ABI-11）。这三项同属 arm32 移植面，但彼此独立。
- 关闭条件：arm32 上 FP 上下文切换用例通过；或明确降级为不支持并写入平台契约。
- 诊断：[notes/core-arm32-fp.md](notes/core-arm32-fp.md)。


### CORE-DOCS-01 翻译器使用手册中的内核豁免机制

- 现状：`docs/exec-xlator/01-usage.md` 的 W^X 段落在验证命令之间解释 task_t.xlator_host、mm_wx_filter_prot 与 re-exec 设置时机。
- 缺口：使用路径混入内核内部字段与实现调用关系。
- 关闭条件：使用页保留 a20.wx 的配置、显式 deny 的验证命令、预期日志和拒绝语义；字段与 re-exec 内部契约移到已有 `03-internals.md` 或源码注释，并从使用页链接。
- 来源：DOCS-03 用户流程边界审查；本包只提出请求，由 CORE owner 修改。

### CORE-DOCS-02 阻塞、信号和超时边界断言仍指向旧实现

- 现状：DOCS 包复验六份归档的关联静态门禁时，归档标记均存在，但 `check-blocking-point-boundary` 有 4 条断言失败：current.c 的 cpu_id 写入不在白名单、task.c 注释被 proc_make_ready 调用规则命中，以及 futex waiter、wait timer 的旧 C 结构已迁到 Rust。`check-signal-exit-boundary` 缺少 park.c 中的旧 PARK_SIGNAL_MODE_PROTOCOL 标记；`check-timeout-ownership-boundary` 有 3 条断言仍查找旧 C 定时器结构、重复拒绝计数器和批量唤醒形态。
- 缺口：这些静态门禁没有跟随 CORE 所有权接口迁移；当前反模式白名单是否仍合理、对应 Rust 结构与跨 FFI 唤醒是否维持原契约，需要由 CORE owner 判断。
- 关闭条件：依据当前状态写入边界、Rust waiter/定时器所有权与唤醒序号契约，更新 `tools/gates.toml` 中对应断言并保留有区分力的负控，然后重跑三个静态门禁及其 runtime 依赖。不要通过放宽全局白名单或删除约束来消除红灯。
- 来源：DOCS 包 DOCS-02 归档引用复验；本包只报告具体失败，不替 CORE 判定锁与生命周期契约。
