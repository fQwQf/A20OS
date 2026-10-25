# 多架构与 SMP 实现、运行验证（2026-10-25）

此轮在 `codex/hypervisor-multiarch` worktree 上继续实现 Intel VMX、ARM32、RV32，
并将可运行后端扩展为并行多 vCPU。LoongArch LVZ 保留显式实验开关，默认不开放；
开发机和 QEMU TCG 均不能执行其 LVZ 正控，用户已确认没有可用 LVZ 硬件。

## 实际执行结果

| 后端 | 实际执行环境 | 双宿主 CPU / 双 guest vCPU | 扩展关闭负控 |
|---|---|---|---|
| Intel VMX/EPT | 上游 Bochs 3.0，VMX=2 软件 CPU | 通过，`peak=2` | 通过，SKIP 并关机 |
| AMD SVM/NPT | QEMU TCG；AMD 嵌套 KVM | 两种加速器均通过，`peak=2` | 通过，SKIP 并关机 |
| AArch64 EL2 | QEMU Cortex-A57，`virtualization=on` | 通过，`peak=2` | 通过，EL1-only SKIP 并关机 |
| ARM32 HYP | QEMU Cortex-A15，`virtualization=on` | 通过，`peak=2` | 通过，SVC-only SKIP 并关机 |
| RV32 H | QEMU RV32 H/Sstc，默认 OpenSBI v1.6 | 通过，`peak=2` | 通过，H=false SKIP 并关机 |
| RV64 H | QEMU RV64 H/Sstc，默认 OpenSBI | 通过，`peak=2` | 通过，H=false SKIP 并关机 |
| LoongArch LVZ | 本地 QEMU 不具备 LVZ | **未验证** | 本地仅验不可用路径 |

这些是实际 guest 指令和虚拟化入口/退出的运行结果，没有把编译或 host harness
记作 guest 执行。VMX 通过 VMXON/VMCS/VMLAUNCH/VMEXIT/EPT 的软件 CPU 执行；
开发机是 AMD，**没有 Intel 实机验证**。LoongArch 没有 LVZ 运行证据。

所有可执行后端均执行三轮独立 guest 创建、装载、输出 `HYP`、关机和销毁。
新后端还验证非页起始地址的第二页 demand fill、guest RAM 读回、屏蔽中断的
无限循环抢占和 STOP。NX/RO 负例实际执行禁止取指/写入，要求 guest FAULT，
只读页内容不变，销毁后 VM/vCPU/映射/页表计数恢复基线。

SMP 测试创建一台 managed VM 的两个 vCPU，使用两个固定在不同宿主 CPU 的
内核 worker 调用 RUN。两个 guest 各写一个 ready 字，等待宿主 release，
宿主必须观察到 ready[0]=ready[1]=1 且同时 RUN 的峰值为 2，才释放 guest。
两个 guest 随后通过 HSM/PSCI/自定义 CPU_OFF 退出；控制器等待并回收 worker，
销毁所有引用并检查资源计数。因此这项测试不能靠单核轮流执行两个 guest 通过。
次 vCPU 的启动由宿主控制器调用公共 hart_start；guest CPU_ON/HSM 启动包装
已实现，但本轮没有独立把这些 guest 启动调用记作已执行验收。

RV32 另有连续三次完整 SMP 重建启动流程的通过记录。当前验收数量是 2，
不是任意 CPU 数量或长时间压力稳定性。既有完整 RV64 OS guest 的 SMP trap
frame 问题与长稳缺口不因此关闭。

## 实现边界

- VMX/SVM 使用每 vCPU 的独立硬件状态，支持 flat32 guest；尚无虚拟 APIC。
  VMX 还要求 EPT、unrestricted guest、INVEPT 和 preemption timer。
- ARM32 增加真正的宿主 secondary 入口、每 CPU 栈、PSCI、GIC SGI、per-CPU
  trap 状态和 HYP monitor；宿主短描述符页表与 guest 64 位 LPAE 页表独立。
- RV32 增加宿主 SBI HSM secondary、跨 hart 64 位原子锁、IPI/RFENCE、VS/FP
  状态和 RV32 双半计时器 CSR；Sv32x4 完整覆盖 34 位 GPA 的四页根表。
- AArch64 增加 PSCI CPU_ON/AFFINITY_INFO、每 vCPU MPIDR 和并行激活。
- native stage-2 允许并发新增映射，但不允许替换已有叶子；每次硬件 re-entry
  都失效翻译。远端仍有活跃 vCPU 时 unmap 返回 EBUSY，防止旧翻译引用已释放帧。
  RV32/RV64 保留 firmware 同步 HFENCE；所有后端拒绝出借页表帧和重复出借帧。
- ARM32 使用 `HYP_PROT_EXEC` 表达 guest 执行权限，保留宿主 `PTE_X=0` 的既有语义。
  元数据至少容纳 512 个 native descriptor，64 位 PTE 原子发布避免 ARM32 撕裂。
- ARM/LoongArch 的虚拟中断控制器、x86 虚拟 APIC、完整 Linux/Windows/A20OS
  guest SMP 引导、物理 ARM/RISC-V 板和长稳压测仍不在本轮通过记录内。

## 重现

`--build` 从当前 worktree 源码构建，默认 Werror；`--cpus 2` 同时启用宿主 SMP
构建与 guest 并行测试。ARM32 需要 `arm-linux-gnueabihf-gcc`；本机使用解包在
`/tmp/a20-arm-toolchain/root` 的 Debian GCC 12，PATH 和 LD_LIBRARY_PATH 指向
其 `usr/bin` 与 `usr/lib/x86_64-linux-gnu`，没有安装全局系统包。

```sh
python3 tools/check_hyp_multiarch.py --arch arm32 --cpus 2 --build
python3 tools/check_hyp_multiarch.py --arch riscv32 --cpus 2 --build
python3 tools/check_hyp_multiarch.py --arch aarch64 --cpus 2 --build
python3 tools/check_hyp_multiarch.py --arch riscv64 --cpus 2 --build
python3 tools/check_hyp_multiarch.py --arch x86_64 --cpus 2 --build --accel tcg
python3 tools/check_hyp_multiarch.py --arch x86_64 --cpus 2 --build --accel kvm
# 对上述每项加 --without-virtualization 运行负控。
python3 tools/check_hyp_multiarch.py --arch loongarch64 --cpus 2 --build --without-virtualization
python3 -m unittest tools.tests.test_hyp_s2 tools.tests.test_hyp_backend_contract -v
```

Intel VMX 的 Bochs 编译和验证命令见
[x86 后端说明](../../kernel/arch/x86_64/hyp/README.md)。从同一源码构建双 CPU
`kernel.bin` 后运行：

```sh
python3 kernel/arch/x86_64/hyp/check_vmx_bochs.py \
  --bochs-dir .kernel-build/bochs/Bochs-REL_3_0_FINAL/bochs \
  --kernel PATH/kernel.bin --cpus 2 --output .kernel-build/vmx-bochs-final
# 加 --without-virtualization 验证 VMX 关闭路径。
```

QEMU 日志在 `.kernel-build/smoke/hyp-*.log`；Bochs 保存 serial、配置、loader 和
调试日志。日志目录被 Git 忽略，可用上述命令重建。
本轮各最终镜像与日志的 SHA-256、实际 PASS/SKIP 标记保存在
[验证证据清单](validation-2026-10-25.json)。相关单元测试共 23 项通过，
`make check-trim-registry` 通过。ARM32、RV32、AArch64、x86 的用户态
`hyp_test` 也完成编译和链接；此项未记作用户态 syscall 运行验收。
其它资料：[ARM32](../../kernel/arch/arm32/hyp/README.md)、
[RV32](../../kernel/arch/riscv32/hyp/README.md)、
[AArch64](../../kernel/arch/aarch64/hyp/README.md)。
