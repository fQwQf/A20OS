# 内核调试技术参考

本页描述调试状态、ABI 适配和符号表接线。GDB 操作、日志判读与用户态调试入口见
[调试指南](../dev/debug.md)。

## 内核调试状态机 {#debug-state}

`kernel/proc/debug.c` 提供与 ABI 无关的内核调试接口（观察者-被观察者模型）：`proc_debug_traceme/attach/detach/resume/singlestep/kill`、寄存器文件读写、地址空间 PEEK/POKE、siginfo 快照、PT_DEBUG_EVENT_EXEC/EXIT 事件停止，以及 syscall 边界停止（`proc_debug_syscall_entry/exit`）。

Linux ABI 的 `ptrace(2)` 是这些接口的薄包装（`kernel/abi/linux/sys_ptrace.c`），请求号与 `struct user_regs_struct` 的转换全部在 ABI 层完成；内核内部层不依赖任何 Linux 常量。Native ABI 的调试对象也映射到同一接口面，见 [Native 调试接口](../native-abi/06-security.md)。

停止语义（对应 task 状态机）：

- 被观察任务在信号投递边界进入 ptrace 停止（`proc_sched_stop_for_debug`，`proc/sched.c` 持有状态转换），观察者可用不带 `WUNTRACED` 的 `wait4` 报告；
- 停止期间寄存器快照在 `ptrace_saved_ctx`，`PTRACE_SETREGS` 等修改在恢复时折回陷阱上下文；syscall 入口停止在恢复时由架构层回卷 EPC 重新执行；
- `PTRACE_CONT` 带信号恢复时通过一次性 `ptrace_deliver_sig` 标记避免二次停止。

Native ABI 的 `A20_OBJ_DEBUG` 会话复用同一状态机，syscall 包装位于
`kernel/abi/native/sys_native_debug.c`，用户态 SDK 位于 `user/liba20rt/a20_debug.h`。
`debug_resume` 的 CONT/SYSCALL 模式对应内核 `PT_DEBUG_RESUME_*`。
native 线程共享信号状态，因此进程级 SIGSTOP 会停住整个进程；操作页据此要求附加独立进程。

## 符号表与回溯接线 {#symbolization}

内核构建采用两遍链接：第一遍产出 `kernel-nosyms.elf`，`tools/gen_kallsyms.py`（纯 stdlib ELF 解析）提取 `.text` 范围内的符号生成紧凑符号表（`kernel/core/kallsyms.c`，`core/kallsyms.h` 声明 API），第二遍把符号表对象重新链接进最终镜像。由于符号表落在 `.rodata`（在 `.text` 之后），两遍的 `.text` 地址一致，表项精确。

`kallsyms_print()` 把地址解析为 `name+0xN`，已接入 `kernel/core/trap.c` 的内核 oops 回溯输出。python3 不可用时符号表为空，`kernel/core/kallsyms.c` 的 weak 定义保证内核仍可链接，回溯退化为裸地址。

## UBSan 运行时与自检 {#ubsan-runtime}

开发构建（BRINGUP=0）默认启用 `-fsanitize=undefined` （`-fno-sanitize=alignment,bounds-strict`），运行时在 `kernel/core/ubsan.c`。任何未定义行为（移位越界、有符号溢出、数组越界等）在启动日志输出 `UBSAN: <kind> at file:line` 后继续运行，便于 smoke 测试暴露隐性 bug。`BRINGUP=1` 默认通过 `CONFIG_UBSAN=0` 关闭；`BRINGUP=0` 的 dev 和发布构建默认启用，除非调用方显式覆盖 `CONFIG_UBSAN=0`。

启动时 `ubsan_selftest()` 输出两行 `UBSAN_SELFTEST: start / PASS`，是**预期的自检**（故意触发一次 handler 验证报告链路，报告文本在自检期间被抑制），不是真实错误；只有当日志出现 `UBSAN: <kind> at ...` 才是真正的未定义行为告警。
