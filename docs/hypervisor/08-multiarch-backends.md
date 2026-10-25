# 多架构 hypervisor 后端（2026-10-25）

本轮在独立 worktree 中增加 AArch64 EL2 和 x86_64 AMD SVM 后端。
它们可以实际执行同架构的小型 guest，复用 Native/Linux 扩展 syscall
背后的 VM/vCPU 生命周期、RAM 装载、控制和资源计数。Linux 扩展 syscall
仍不是 `/dev/kvm`。这些结果不代表完整 guest 操作系统或生产隔离已经验收。

| 宿主架构 | 后端与启动条件 | 本轮范围 |
|---|---|---|
| RISC-V64 | 既有 `riscv-h`，H/Sstc 和 firmware 权限 | 保留既有实现，重新验证映射和三轮 guest 生命周期 |
| AArch64 | `aarch64-el2`，固件经 EL2 交接；QEMU `virt,virtualization=on` | EL1h guest，39 位 IPA、32 位宿主 PA，4 KiB stage-2，FP/SIMD、虚拟计时器上下文，PL011 轮询、PSCI，单 vCPU |
| x86_64 | `x86-svm`，AMD SVM/NPT，firmware 未禁止 SVM | flat 32 位保护模式起步，四级 NPT，GPR/legacy FP/SSE，受限 CPUID/MSR 模拟，单 vCPU；TCG 与 KVM 均实测 |
| x86_64 Intel VMX | 未实现 | 返回不支持 |
| LoongArch64 / RISC-V32 / ARM32 | 尚无可运行的架构后端 | 明确返回不支持，不能把 CPU 扩展存在当作后端可用 |
| PPC64LE / LoongArch32 | 仓库已退役的架构 | 本轮没有恢复为受支持平台 |
| ARMv7-M | Cortex-M MCU，无对应 guest 特权模式 | 不提供硬件 hypervisor |

新后端均不支持 guest SMP、虚拟中断控制器及外部虚拟 IRQ 注入。
AArch64 不支持 SVE；x86 不支持 AVX/XSAVE，不提供完整 PC 设备模型。
`hypvm`、`hyp_boot` 和 supervisor 的 ELF/FDT 装载流程仍面向 RISC-V guest；
本轮适配了 `hyp_test` 的同架构小程序，未把既有 RISC-V OS guest 流程
当作 AArch64/x86 完整 OS 引导支持。

## 实现与隔离边界

- AArch64 的宿主仍在 EL1；boot 保留 MMU 关闭的 EL2 monitor。
  monitor 保存/恢复宿主与 guest 的 EL1/FP 状态，处理真实 entry/exit。
  EL1-only 引导和 NOMMU 配置保持不可用。
- SVM 通过 VMCB、HSAVE、IOPM/MSRPM 和汇编入口执行 guest。每次进入刷新
  ASID 翻译；恢复宿主 GS、控制寄存器和 FP 状态后再开放中断。
  未实现的敏感操作产生 guest fault。
- 新的 `hyp_s2_tree_*` 只共享 radix 结构，硬件 descriptor 由后端编码。
  继续使用 `pt_meta_t`、guest frame 标志及资源统计；audit 独立解码
  stage-2/NPT，避免误用宿主 stage-1 权限或 user 位语义。
- 每 VM 的 `s2_lock` 与 RUN 激活同步。本轮后端不允许远端 CPU 已激活时
  修改映射；当前退出上下文可以补页，每次重新激活都会失效旧翻译。
  映射独占 guest frame，拒绝把宿主页表页或已出借页再次映射。
- vCPU 架构私有状态附加在原有汇编布局之后，最后一个引用释放时调用
  backend destroy。VM 创建要求 load/enter/put 等必要回调完整。
- 宿主物理定时中断可以打断 guest 屏蔽中断后的紧循环；RUN 返回 YIELD
  后可继续 RUN 或 STOP，不依赖 guest 自愿执行 hypercall。

架构细节和官方资料见
[AArch64 后端](../../kernel/arch/aarch64/hyp/README.md) 与
[SVM 后端](../../kernel/arch/x86_64/hyp/README.md)。

## 可重复验证

在本 worktree 内运行，`--build` 会从当前源码构建 BRINGUP 内核并记录构建日志，
然后启动 QEMU；不借用主 checkout 的镜像。首次构建需要相应交叉工具链和 Rust target。

```sh
python3 tools/check_hyp_multiarch.py --arch aarch64 --build
python3 tools/check_hyp_multiarch.py --arch aarch64 --build --without-virtualization
python3 tools/check_hyp_multiarch.py --arch x86_64 --build
python3 tools/check_hyp_multiarch.py --arch x86_64 --build --accel kvm
python3 tools/check_hyp_multiarch.py --arch x86_64 --build --without-virtualization
python3 tools/check_hyp_multiarch.py --arch riscv64 --build
python3 tools/check_hyp_multiarch.py --arch riscv64 --build --without-virtualization
python3 -m unittest tools.tests.test_hyp_s2 tools.tests.test_hyp_backend_contract -v
```

也可用 `--kernel <path>` 指定已经构建的内核。AArch64 必须提供 `kernel.bin`，
让 QEMU 按 raw Linux boot 协议传递 DTB/bootargs；其它两个架构使用 `kernel.elf`。
x86 测试工具通过 fw_cfg 传入 `a20.hyp_test=1`，并选 `pc` machine 避免
空 AHCI 控制器扫描延迟。KVM 正控需要开发机能向 A20 暴露嵌套 AMD SVM。

正控要求 `HYP_SELFTEST: PASS` 和 `HYP_ARCH_TEST: PASS`，还要求宿主正常关机。
AArch64/x86 额外要求 `preemption+stop PASS`。自测执行三次输出 HYP 并关机的
真实 guest，验证第二页内偏移 4 字节的 demand fill、guest RAM 读回、销毁后
VM/vCPU/映射/页表计数归零；随后运行屏蔽中断的无限循环 guest，验证抢占和 STOP。
关闭扩展的负控必须 SKIP 并关机，不能用 SKIP 冒充正控通过。

本轮上述硬件正负控均通过，包括 x86 TCG/KVM。
单元 harness 直接编译生产 S2/VM admission/stub C 代码，覆盖两种 descriptor、
末页边界、坏 PFN、重复映射、跨 VM alias、元数据/内存分配失败、远端活跃拒绝、
失效失败时的帧保留和销毁回收。harness 不模拟硬件 TLB，硬件行为由 QEMU 检查。
日志位于 `.kernel-build/smoke/hyp-*.log`；这些文件被忽略，可通过上述命令重新生成。

`hyp_test` 的 AArch64/x86 用户程序已交叉编译；硬件运行证据来自 boot 自测，
本轮没有把两种 ABI 的用户态运行矩阵或长期压力测试记作通过。
既有 RISC-V 多 VM 重复运行和 SMP 问题仍见
[平台状态](../status/current/hypervisor-implementation-status.md)。

本轮未记为通过的仓库门禁：`make check-arch-boundary` 的前置全架构 MMU
矩阵在 ARM32 构建失败（开发机缺 `arm-linux-gnueabihf-gcc`）；直接运行
`python3 tools/gates.py check-arch-boundary` 也发现基线已有的三处架构条件分支
（`kernel/main.c` 一处、`kernel/abi/linux/sys_proc.c` 两处，HEAD 中同样存在）。
本轮没有修改这些既有分支，也没有把失败门禁写成通过。
