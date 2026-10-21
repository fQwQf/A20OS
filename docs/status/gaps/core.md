# CORE 缺口

对应 V1-CORE。现状见 [current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### CORE-07 arm32 的 FP 上下文

- 现状：arm32 现在**能编过也能启动**（本包完成：`a20os-buildenv` 容器里 `make ARCH=arm32 ABI=linux BRINGUP=0 dev-build` 构建，QEMU virt/cortex-a15/1G 启动到 shell、执行命令并干净关机）。但用户态 FP 上下文仍未保存：`trap_context_t` 只有 84 字节 GPR 部分，没有 `d0-d15`/FPSCR，所以两个任务共享同一份物理 FP 寄存器文件。附带的取证：`user/cmds/core/fp_touch.c` 证明 EL0 的 VFP 本身可用（`FP_TOUCH: PASS`，CPACR/FPEXC 在 `arch/arm32/boot/entry.S` 已开）；`fork_fpu_probe.c` 的 arm32 分支（父进程装 sentinel → 让子进程装另一组 → 父进程读回自己的）已写好，用自旋代替 `usleep`，因为 **arm32 的 `sleep`/`nanosleep` 根本不唤醒**（见下）。在 trap 帧尾部加 VFP 保存/恢复的尝试（TF_SIZE 84→224、保存/恢复 + 帧拷贝循环）会让第一次用户态 syscall 之后挂死，未定位，改动已撤回。
- 缺口：arm32 上 FP 上下文切换没有实现也没有用例；另外 arm32 的等待/超时路径本身有缺陷（`sleep 1` 永不返回），这是同一个移植面上的独立问题。
- 关闭条件：arm32 上 FP 上下文切换用例通过；或明确降级为不支持并写入平台契约。
- 诊断：[notes/core-arm32-fp.md](notes/core-arm32-fp.md)。

