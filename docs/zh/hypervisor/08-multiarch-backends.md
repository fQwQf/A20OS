# 多架构 hypervisor 后端

当前共享 VM/vCPU 核心提供六条架构后端路径，复用 Native/Linux 扩展 syscall 背后的生命周期、RAM 装载、控制和资源计数。Linux 扩展 syscall 仍不是 `/dev/kvm`。详细范围与验证数据见 [SMP 验证记录](09-smp-validation.md)。

| 后端 | guest 与映射 | 本地实际执行 |
|---|---|---|
| RV64 H | VS-mode、Sv39x4 | QEMU，两并行 vCPU 通过 |
| RV32 H | VS-mode、完整 34 位 GPA Sv32x4 | QEMU，两并行 vCPU 通过 |
| AArch64 EL2 | EL1h、39 位 IPA / 32 位宿主 PA | QEMU，两并行 vCPU 通过 |
| ARM32 HYP | ARM SVC、64 位 LPAE stage-2 / 32 位 IPA | QEMU，两并行 vCPU 通过 |
| AMD SVM / Intel VMX | flat32、NPT / EPT | SVM QEMU TCG/KVM 和 VMX Bochs，两并行 vCPU 通过 |
| LoongArch LVZ（实验） | 整数 guest、GCSR / GPA root | 仅编译和无 LVZ 负控通过；默认关闭，**没有正控执行证据** |

VMX 在 Bochs 上真实执行 VMX/EPT 指令；当前开发机是 AMD，没有 Intel 实机验证。LoongArch TCG 不实现 LVZ，用户已确认没有 LVZ 硬件；不能把其通过编译或 QEMU SKIP 写成 LVZ guest 已能运行。

新 SMP 路径验证两个 guest 在两个宿主 CPU 上同时 RUN、共享内存会合、CPU_OFF 以及资源回收。测试还执行最高 GPA 根槽、按需补页、真实 NX/RO 权限拒绝、屏蔽中断的紧循环抢占和 STOP。数量、环境与未覆盖部分以验证记录为准。完整操作系统 guest SMP、virtual GIC/APIC、物理 ARM/RISC-V 板和长稳验收未完成。既有 RISC-V 完整 OS guest 的 SMP trap frame 问题仍在台账中。

native radix 后端不复用宿主 PTE 解码。它们保留 per-page metadata、guest 帧标志与独立 audit，使用原子的 64 位硬件 descriptor，支持 ARM32 短描述符宿主。并行新增映射允许零→有效发布，每次 guest re-entry 都失效翻译；远端 vCPU 仍活跃时 unmap 拒绝，防止释放仍可能被访问的 backing。RISC-V 用 firmware 同步 HFENCE。所有映射拒绝页表帧和重复出借帧。

`hyp_test` 已适配各可执行 ISA，x86 根据 CPU 能力选择 VMMCALL/VMCALL。`hypvm`、`hyp_boot` 和 supervisor 的完整 ELF/FDT 装载流程仍面向 RISC-V OS guest；这些流程没有自动获得 ARM/x86 完整 OS 引导支持。PPC64LE/LoongArch32 仍是仓库退役架构；ARMv7-M 不提供对应硬件 guest 特权模式。

实现与官方架构资料：[AArch64](../../../kernel/arch/aarch64/hyp/README.md)、[ARM32](../../../kernel/arch/arm32/hyp/README.md)、[RV32](../../../kernel/arch/riscv32/hyp/README.md)、[x86](../../../kernel/arch/x86_64/hyp/README.md)、[LoongArch](../../../kernel/arch/loongarch64/hyp/README.md)。

复测从 [SMP 验证记录](09-smp-validation.md) 中的命令开始。新源码和镜像均位于独立 worktree，未把主 checkout 的预编译内核当作通过证据。
