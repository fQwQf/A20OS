# 调试 A20OS

本文档介绍如何调试 A20OS。调试信息主要来自串口输出、QEMU 日志和 GDB。

## QEMU + GDB 源码级调试

`make debug-<arch>` 会用调试选项编译并启动 QEMU，等待 GDB 连接：

```bash
make debug-riscv64
# 也可替换为 debug-loongarch64 / debug-arm64 / debug-x86_64 / debug-arm32 / debug-riscv32 / debug-ppc64le
```

在另一个终端启动 GDB：

```bash
gdb-multiarch .kernel-build/riscv64-qemu-virt-riscv64-linux-bringup/kernel.elf(gdb) target remote :1234(gdb) break panic(gdb) continue
```

- 默认使用端口 `1234`。
- 若 `BRINGUP=0`，ELF 路径为 `...-dev/kernel.elf`；若 `BRINGUP=1`，路径为 `...-bringup/kernel.elf`。

常用 GDB 命令：

```text
(gdb) info registers          # 查看寄存器(gdb) bt                        # 尝试回溯(gdb) x/i $pc                   # 反汇编当前 PC(gdb) x/32i $pc-32              # 查看崩溃附近指令(gdb) print *task               # 查看任务结构体（若符号可用）
```

## 串口日志与 panic 输出

A20OS 使用 UART 输出启动日志。panic 实现在 `kernel/core/panic.c` 中，会把消息打印到串口后尝试关机。

日志来源：

- 手动运行：`make run-*` 直接把串口输出打印到终端。
- smoke 测试：日志保存在 `.kernel-build/smoke/<target>.log`。
- 评测运行：日志保存在 `.eval-state/logs/serial-rv.txt` 和 `serial-la.txt`。
- 崩溃时先看日志末尾的 `[PANIC]` 或 `========== KERNEL PANIC ==========` 行。

## 如何阅读一次崩溃

1. 从串口日志找到最后 panic 消息，确认触发模块和大致原因。
2. 在 GDB 中查看 PC 和寄存器，确认崩溃指令。
3. 对页类错误，查看架构相关异常地址寄存器（如 RISC-V 的 `sepc` / `stval`）。
4. 若函数被 `-O3` 内联，调试信息可能不准确，建议用 `make debug-<arch>` 复现。

## 常见构建与运行问题

### 1. `make run-*` 提示 `mkfs.fat` 或 `mcopy` 找不到

**原因**：构建文件系统镜像需要 `dosfstools` / `mtools` / `e2fsprogs`。

**解决**：

```bash
sudo apt install dosfstools mtools e2fsprogs
```

### 2. 设置 `NR_CPUS=2` 后构建失败

**原因**：Makefile 默认阻止多核构建，避免未经验证的 SMP 路径进入产物。

**解决**：

```bash
# 正常开发保持单核
make ARCH=riscv64 run

# 仅在做明确 SMP 实验时打开
make ARCH=riscv64 NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 BRINGUP=1 kernel-only
```

### 3. QEMU 启动后没有串口输出

**原因**：RISC-V 等目标需要 `-bios default` 和 `-global virtio-mmio.force-legacy=false`；手动拼写命令容易遗漏。

**解决**：始终使用 `make run-*` 或 `make debug-*`，不要手写 QEMU 参数。

### 4. `flash-stm32f103-xuanwu` 失败

**原因**：OpenOCD 未安装、CMSIS-DAP 未连接，或接口配置不匹配。

**解决**：

```bash
sudo apt install openocd
# 连接开发板后再执行
make flash-stm32f103-xuanwu
```

### 5. 用户态编译失败

**原因**：`user/external/` 子模块未初始化。

**解决**：

```bash
git submodule update --init --recursive
```

##  注意

- `debug-*` 目标默认使用 `BRINGUP=0`；如只需要内核，请指定 `make ARCH=riscv64 BRINGUP=1 debug-riscv64`。
- 发布构建使用 `-O3`，可能内联或优化变量，导致 GDB 中变量值与源码不一致。
- QEMU 的 `-s` 监听所有网络接口，不要在不可信网络中使用。
- 崩溃时先保存 `.kernel-build/smoke/` 或终端日志，再重新编译，避免覆盖现场。

## 内核调试接口（proc_debug_*）

`kernel/proc/debug.c` 提供与 ABI 无关的内核调试接口（观察者-被观察者模型）：
`proc_debug_traceme/attach/detach/resume/singlestep/kill`、寄存器文件读写、
地址空间 PEEK/POKE、siginfo 快照、PT_DEBUG_EVENT_EXEC/EXIT 事件停止，
以及 syscall 边界停止（`proc_debug_syscall_entry/exit`）。

Linux ABI 的 `ptrace(2)` 是这些接口的薄包装（`kernel/abi/linux/sys_ptrace.c`），
请求号与 `struct user_regs_struct` 的转换全部在 ABI 层完成；内核内部层不
依赖任何 Linux 常量。未来 Native ABI 的调试对象可映射到同一接口面。

停止语义（对应 task 状态机）：
- 被观察任务在信号投递边界进入 ptrace 停止（`proc_sched_stop_for_debug`，
  `proc/sched.c` 持有状态转换），观察者可用不带 `WUNTRACED` 的 `wait4` 报告；
- 停止期间寄存器快照在 `ptrace_saved_ctx`，`PTRACE_SETREGS` 等修改在恢复时
  折回陷阱上下文；syscall 入口停止在恢复时由架构层回卷 EPC 重新执行；
- `PTRACE_CONT` 带信号恢复时通过一次性 `ptrace_deliver_sig` 标记避免二次停止。

已实现（Linux ABI）：TRACEME/ATTACH/DETACH/CONT/SYSCALL/SINGLESTEP(x86_64)、
GETREGS/SETREGS/GETFPREGS/SETFPREGS/GETREGSET(NT_PRSTATUS, NT_FPREGSET)、
PEEKDATA/POKEDATA/PEEKUSER/POKEUSER、GETSIGINFO/SETSIGINFO、SETOPTIONS
(TRACESYSGOOD/TRACEEXEC/TRACEEXIT/EXITKILL)、GETEVENTMSG、KILL。

未实现：PTRACE_SEIZE/INTERRUPT、TRACEFORK/CLONE 事件、SECCOMP、riscv64
单步（与 Linux 一致，gdb 回退断点步进）。

## kallsyms 符号化回溯

内核构建采用两遍链接：第一遍产出 `kernel-nosyms.elf`，
`tools/gen_kallsyms.py`（纯 stdlib ELF 解析）提取 `.text` 范围内的符号生成
紧凑符号表（`kernel/core/kallsyms.c`，`core/kallsyms.h` 声明 API），第二遍
把符号表对象重新链接进最终镜像。由于符号表落在 `.rodata`（在 `.text` 之后），
两遍的 `.text` 地址一致，表项精确。

`kallsyms_print()` 把地址解析为 `name+0xN`，已接入 `kernel/core/trap.c` 的
内核 oops 回溯输出。python3 不可用时符号表为空，`kernel/core/kallsyms.c`
的 weak 定义保证内核仍可链接，回溯退化为裸地址。

## 已知基核问题（与调试接口无关）

fork + 按需分页的路径存在偶发（对某些二进制尺寸近乎确定）的物理页复用竞态：
子进程文本页可能在停止期间被回收复用，表现为子进程文本被写入栈类数据后
SIGSEGV。`user/cmds/core/ptrace_smoke.c` 因此在 poke 与恢复之间加入短延时
规避该竞态窗口；根因在页分配/引用计数层，与 ptrace 实现无关（A20 团队正在
修复同类 0x63636363 问题）。
