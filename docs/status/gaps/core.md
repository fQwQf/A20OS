# CORE 缺口

对应 V1-CORE。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### CORE-07 arm32 的 FP 上下文

- 现状：ARM32 trap 帧保存 `d0-d15`/FPSCR，调度帧保存 AAPCS callee-saved `d8-d15`/FPSCR，信号附加帧保存/恢复 VFP。`RAMFS_FPU_TESTS=1` 下的 `/bin/fork_fpu_probe` 已在 QEMU 64 MiB 验证父子进程全部 16 个 VFP 寄存器隔离。
- 运行时范围：64 MiB QEMU virt 已连续 10 轮通过每轮 40 次顺序 fork/exec、FPU 隔离、抢占和空闲唤醒；[ABI-11](abi-ipc.md) 的原关闭条件已满足。这不等于 V3s 实机或 ARM32 媒体驱动验收。
- 验收入口：`tools/check_serial_shell.py --arch arm32 --kernel <kernel.elf> --memory-mb 64 --fpu --timers`。FP 信号状态的实现仍需要真实用户态信号压力覆盖。
- 历史诊断：[notes/core-arm32-fp.md](notes/core-arm32-fp.md)，其中“未保存 VFP”属于修复前现场。


### CORE-RUNTIME-05 NOMMU 用户态在 fork 出的子进程结束后退出

- 现状：`ARCH=riscv64 ABI=both NOMMU=1` 的 QEMU 镜像能启动到 mksh，内建命令正常；执行任意会 fork 的输入（`/bin/echo ...` 或 `(echo SUB)`）后子进程输出正常，随后 mksh 退出、init 打印 `[init] shutting down`，父 shell 不再接受下一条命令。没有用户态 FATAL/SIGSEGV 记录。这是独立的 NOMMU 生命周期缺口；ARM32 [ABI-11](abi-ipc.md) 的 MMU 运行时修复不构成此路径的验证。
- 缺口：NOMMU 用户态的 fork/reap 生命周期没有完成；因此 NOMMU 只能验证到第一次外部 ELF exec，不能验证多命令运行时。
- 关闭条件：RV64 NOMMU 镜像里连续执行两次 `/bin/echo` 后 mksh 仍存活，且 `poweroff` 走正常关机路径。
- 来源：RUNTIME 包在新增 `smoke-nommu-exec` guest 运行时发现；本包只覆盖 ELF/exec，未改 fork/reap 实现。
