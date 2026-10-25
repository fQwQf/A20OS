# CORE 缺口

对应 V1-CORE。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### CORE-07 arm32 的 FP 上下文

- 现状：arm32 现在**能编过也能启动**。内核镜像（容器内 `make ARCH=arm32 ABI=linux BRINGUP=1 dev-build`，QEMU virt/cortex-a15/1G）跑到 `part ok` 并干净关机（rc=0）；用户态镜像（`ABI=both BRINGUP=0`）也能启动到 shell，`echo` 等内建命令可用，但 fork 出去的命令（`/bin/echo`、`poweroff`）返回后 mksh 空指针崩溃（见 [ABI-11](abi-ipc.md)），所以用户态镜像还不能干净关机。用户态 FP 上下文仍未保存：`trap_context_t` 只有 84 字节 GPR 部分，没有 `d0-d15`/FPSCR，两个任务共享同一份物理 FP 寄存器文件。取证：`user/cmds/core/fp_touch.c` 证明 EL0 的 VFP 本身可用（`FP_TOUCH: PASS`，CPACR/FPEXC 在 `arch/arm32/boot/entry.S` 已开）；`fork_fpu_probe.c` 的 arm32 分支（父进程装 sentinel → 让子进程装另一组 → 父进程读回自己的）已写好，用自旋代替 `usleep`，因为 **arm32 的 `sleep`/`nanosleep` 根本不唤醒**。在 trap 帧尾部加 VFP 保存/恢复的尝试（TF_SIZE 84→224、保存/恢复 + 帧拷贝循环）会让第一次用户态 syscall 之后挂死，未定位，改动已撤回。
- 缺口：arm32 上 FP 上下文切换没有实现也没有用例；arm32 的等待/超时路径本身有缺陷（`sleep 1` 永不返回）；fork 出去的命令返回后 shell 崩溃（ABI-11）。这三项同属 arm32 移植面，但彼此独立。
- 关闭条件：arm32 上 FP 上下文切换用例通过；或明确降级为不支持并写入平台契约。
- 诊断：[notes/core-arm32-fp.md](notes/core-arm32-fp.md)。


### CORE-RUNTIME-05 NOMMU 用户态在 fork 出的子进程结束后退出

- 现状：`ARCH=riscv64 ABI=both NOMMU=1` 的 QEMU 镜像能启动到 mksh，内建命令正常；执行任意会 fork 的输入（`/bin/echo ...` 或 `(echo SUB)`）后子进程输出正常，随后 mksh 退出、init 打印 `[init] shutting down`，父 shell 不再接受下一条命令。没有用户态 FATAL/SIGSEGV 记录。该现象与 [ABI-11](abi-ipc.md) 记录的 arm32 “fork 出的命令返回后 mksh 崩溃”属同一等待/信号返回面，但 NOMMU RV64 上是直接退出而不是空指针崩溃。
- 缺口：NOMMU 用户态的 fork/reap 生命周期没有完成；因此 NOMMU 只能验证到第一次外部 ELF exec，不能验证多命令运行时。
- 关闭条件：RV64 NOMMU 镜像里连续执行两次 `/bin/echo` 后 mksh 仍存活，且 `poweroff` 走正常关机路径。
- 来源：RUNTIME 包在新增 `smoke-nommu-exec` guest 运行时发现；本包只覆盖 ELF/exec，未改 fork/reap 实现。
