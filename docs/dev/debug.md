# 调试 A20OS

A20OS 的调试信息主要来自串口输出、QEMU 日志和 GDB。

## QEMU + GDB 源码级调试

`make debug-<arch>` 会用调试选项编译并启动 QEMU，等待 GDB 连接：

```bash
make debug-riscv64
# 也可替换为 debug-loongarch64 / debug-arm64 / debug-x86_64 / debug-arm32 / debug-riscv32 / debug-ppc64le
# 这些目标是实例系统的薄包装；等价的实例写法：tools/a20 debug qemu-riscv64（见 instances.md）
```

在另一个终端启动 GDB：

```bash
gdb-multiarch .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf
(gdb) target remote :1234
(gdb) break panic
(gdb) continue
```

- 默认使用端口 `1234`。
- 默认 `ABI=both BRINGUP=0`，因此 RISC-V64 ELF 如上。若显式使用 `BRINGUP=1`，对应路径是 `.kernel-build/riscv64-qemu-virt-riscv64-both-bringup/kernel.elf`；其他 ABI、SMP、NOMMU 或 embedded 组合会改变 variant 后缀。

常用 GDB 命令：

```text
(gdb) info registers          # 查看寄存器
(gdb) bt                      # 尝试回溯
(gdb) x/i $pc                 # 反汇编当前 PC
(gdb) x/32i $pc-32            # 查看崩溃附近指令
(gdb) print *task             # 查看任务结构体（若符号可用）
```

## 串口日志与 panic 输出

A20OS 使用 UART 输出启动日志；panic 会把消息打印到串口后尝试关机。

日志来源：

- 手动运行：`make run-*` 直接把串口输出打印到终端。
- smoke 测试：日志保存在 `.kernel-build/smoke/<target>.log`。
- 崩溃时先看日志末尾的 `[PANIC]` 或 `========== KERNEL PANIC ==========` 行。

## 如何阅读一次崩溃

1. 从串口日志找到最后 panic 消息，确认触发模块和大致原因。
2. 在 GDB 中查看 PC 和寄存器，确认崩溃指令。
3. 对页类错误，查看架构相关异常地址寄存器（如 RISC-V 的 `sepc` / `stval`）。
4. 若函数被 `-O3` 内联，调试信息可能不准确，建议用 `make debug-<arch>` 复现。

## 常见构建与运行问题

### 1. `make run-*` 提示 `mkfs.fat` 或 `mcopy` 找不到

构建文件系统镜像需要 `dosfstools` / `mtools` / `e2fsprogs`，装上即可：

```bash
sudo apt install dosfstools mtools e2fsprogs
```

### 2. 未验证平台设置 `NR_CPUS=2` 后构建失败

Makefile 只允许 RISC-V64、AArch64、LoongArch64 和 x86_64 的同名 QEMU virt 板直接使用多核；其他平台默认拒绝未经验证的 SMP 产物。

```bash
# 已验证的 RISC-V64 QEMU SMP 不需要额外开关
make ARCH=riscv64 BOARD=qemu-virt-riscv64 NR_CPUS=2 run

# 未验证平台仅在明确 bring-up 实验中放行
make ARCH=ppc64le BOARD=qemu-virt-ppc64le NR_CPUS=2 \
    ALLOW_UNVERIFIED_SMP=1 BRINGUP=1 kernel-only
```

### 3. QEMU 启动后没有串口输出

RISC-V 等目标需要 `-bios default` 和 `-global virtio-mmio.force-legacy=false`，手动拼写命令容易遗漏。始终使用 `make run-*` 或 `make debug-*`，不要手写 QEMU 参数。

### 4. STM32 自动运行或烧录入口失败

`run-stm32f103-qemu` 和 `flash-stm32f103-xuanwu` 仍引用加入 `BOARD` 前的旧产物目录，与实际 `BUILD_DIR` 不一致。安装 QEMU/OpenOCD 或重新连接 CMSIS-DAP 不能修复这个 launcher 问题。

先只使用 `make stm32f103-bringup` 或 `make stm32f103-xuanwu` 构建固件，并按 [STM32F103 移植说明](../platforms/stm32f103-port.md) 核对实际产物。修复 Makefile recipe 前，不要把自动运行或烧录目标作为通过证据。

### 5. 用户态编译失败

`user/external/` 子模块未初始化：

```bash
git submodule update --init --recursive
```

## 注意

- `debug-*` 目标默认使用 `BRINGUP=0`；如只需要内核，请指定 `make ARCH=riscv64 BRINGUP=1 debug-riscv64`。
- 发布构建使用 `-O3`，可能内联或优化变量，导致 GDB 中变量值与源码不一致。
- QEMU 的 `-s` 监听所有网络接口，不要在不可信网络中使用。
- 崩溃时先保存 `.kernel-build/smoke/` 或终端日志，再重新编译，避免覆盖现场。

## 用户态进程调试

Linux ABI 的 `ptrace(2)` 可用于附加、停止/恢复、读写寄存器与地址空间、观察 exec/exit 事件及 syscall 边界。观察者可用不带 `WUNTRACED` 的 `wait4` 接收被调试任务的停止通知。支持的请求与边界如下：

已实现（Linux ABI）：TRACEME/ATTACH/SEIZE/INTERRUPT/DETACH/CONT/SYSCALL/SINGLESTEP(x86_64)、 GETREGS/SETREGS/GETFPREGS/SETFPREGS/GETREGSET(NT_PRSTATUS, NT_FPREGSET)、 PEEKDATA/POKEDATA/PEEKUSER/POKEUSER、GETSIGINFO/SETSIGINFO、SETOPTIONS (TRACESYSGOOD/TRACEEXEC/TRACEEXIT/EXITKILL)、GETEVENTMSG、KILL。

未实现：TRACEFORK/CLONE 事件、SECCOMP、riscv64 单步（与 Linux 一致，gdb 回退断点步进）。

内核调试接口、寄存器快照与 syscall 恢复机制见 [内核调试技术参考](../kernel/debugging-internals.md#debug-state)。

## 符号化回溯

内核 oops 回溯可显示 `name+0xN`。若只有裸地址，可结合本次构建的 `kernel.elf` 在 GDB 中定位；符号表生成与降级条件见 [符号化技术参考](../kernel/debugging-internals.md#symbolization)。

## Native ABI 调试接口（Debug 0x0900）

Native ABI 的调试操作使用 `A20_OBJ_DEBUG` 会话 handle；用户态 SDK 入口是 `user/liba20rt/a20_debug.h`，API 与权限见 [安全模型](../native-abi/06-security.md)。使用时注意：

- 会话是 handle（可收窄权限、随进程消亡自动释放），不是 pid；
- `debug_wait` 同时报告停止事件与**退出事件**（目标变 zombie 时报告 EXIT 事件，消息为退出码），无需 TRACEEXIT 选项；
- 无信号注入（native 无信号概念）；`debug_resume` 支持 CONT/SYSCALL 模式；
- `debug_traceme` 与 `debug_attach` 互斥（同 Linux）；
- 进程级 SIGSTOP 会停住整个进程，因此调试器应 attach 独立进程。

权限（[Native ABI 安全模型](../native-abi/06-security.md) §8.1）：READ/WRITE/WAIT/SIGNAL/CONTROL/ADMIN 对应各 debug 操作；spawn/thread_create 的 task handle 自带 ADMIN。

验证：`smoke-native-debug`（riscv64 QEMU，`/bin/native-debug-rv`）。

## UBSan（未定义行为检测）

开发构建（`BRINGUP=0`）默认启用 `-fsanitize=undefined`（排除 `alignment,bounds-strict`）。检测到移位越界、有符号溢出等问题时，会输出 `UBSAN: <kind> at file:line` 并继续运行。`BRINGUP=1` 默认关闭；可用 `CONFIG_UBSAN=0` 显式关闭其他构建中的检测。

启动时的 `UBSAN_SELFTEST: start / PASS` 是预期自检，不是真实错误；`UBSAN: <kind> at ...` 才是未定义行为告警。实现与自检接线见 [内核调试技术参考](../kernel/debugging-internals.md#ubsan-runtime)。
