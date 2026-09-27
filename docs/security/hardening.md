# A20OS 安全加固状态

本文记录内核安全/硬化特性的当前状态、验证入口与已评估但暂缓的项。
最后核实：2026-09（当前工作树）。验证记录均为历史记录，引用为当前
结论前须在当期提交上重新运行。

## 已落地

| 特性 | 实现 | 验证 |
|---|---|---|
| 用户指针校验 | `kernel/mm/mm.c` 的 `copy_from_user`/`copy_to_user` 统一入口：范围检查（`user_range_ok`，防回绕）+ 逐页 PTE_V/PTE_U/可写校验 + COW 折断；无 KERNEL_DS/set_fs 模式 | 全部 syscall 路径强制经过 |
| 监督模式访问防护 | x86_64 `x86_64_enable_smep_smap()`（CPUID leaf 7 门控 CR4.SMEP/SMAP，`trap_init` 每 CPU 调用）；aarch64 `aarch64_enable_pan()`（ID_AA64MMFR1_EL1.PAN 门控 SCTLR_EL1.PAN）；riscv64 用户 trap 入口不再置 SUM（保持 SUM=0，blanket SUM=1 已移除）。内核从不直接解引用用户 VA（全部经直映射拷贝辅助），故无需 stac/clac 窗口；riscv64 侧任何遗漏的监督态用户页访问立即 fault，与 frame.c 取证加固配合成为带现场的 panic | `smoke-x86_64`/`smoke-aarch64`（默认 CPU 空操作路径）；`-cpu max` 完整引导到 mksh 无 fault（防护实际生效路径）；`smoke-riscv64`/`smoke-abi-linux` 验证 riscv64 无隐藏直接解引用 |
| 用户态 W^X | `kernel/mm/wx.c` 单一策略 `mm_wx_filter_prot()`，mmap/mprotect/ELF 装载全路径过滤；默认 deny（-EACCES），cmdline `a20.wx=strip|off` 可降级 | `user/cmds/stress/wx_aslr_test.c`（RWX 拒绝）；全量 195 个静态 musl 二进制扫描零 RWX PT_LOAD |
| 内核自身 W^X（KXAN） | `kernel/arch/{riscv64,x86_64,aarch64}/mm/kwx.c`：`arch_kernel_wx_finalize()` 在 `mm_init()` 后把引导大页旁路重建为 text=ROX/rodata=RO/data=RW+NX、直映射 NX；drvmod 模块 text=RX（`arch_kwx_module_protect`）；aarch64 置回 SCTLR_EL1.WXN | 启动自检（`mm_query_leaf` 四点断言，失败 panic）+ `[KXAN]` 日志行；smoke-riscv64/abi-linux/x86_64/aarch64 + smoke-smp-bringup（2 核）全 PASS。边界见 `docs/security/kernel-wx.md` |
| ASLR | `kernel/mm/aslr.c`：栈顶向下页对齐偏移（64 位 10 位熵，受 vdso_layout 约束）、mmap 基址 per-process 随机（64 位 20 位熵）、brk 起始随机偏移；PIE 基址原有 11 位熵。fork 继承布局（Linux 语义） | `wx_aslr_test`：两次 exec 栈/mmap 地址不同；`cat /proc/self/maps` 两次运行 `[stack]` 区间不同 |
| 内核栈 canary | `kernel/core/stack_protector.c`：`__stack_chk_guard`（熵池就绪后随机化）+ `__stack_chk_fail`；`-fstack-protector-strong` 默认开（`CONFIG_STACK_PROTECTOR`） | 全架构零警告构建；smoke 无 stack smashing |
| seccomp | `kernel/ipc/seccomp.c`：cBPF verifier + STRICT/FILTER 模式 + fork 继承，挂在 syscall 主路径 | `smoke-syscall-ext` |
| capabilities | 15 个 caps 子集（`kernel/include/proc/proc.h`），capset EPERM 矩阵在 `kernel/proc/cred.c` | syscall 覆盖表 |
| IOMMU/DMA 隔离 | RISC-V IOMMU per-device domain 动态 claim/map/unmap + fault queue 消费（fail-closed），`/proc/a20/iommu` 计数器 | `smoke-iommu-udriver-isolation`（授权内成功/窗口外 fault 被消费并阻断） |
| UBSan | 开发 profile 默认 `-fsanitize=undefined`（bringup/benchmark 关闭） | 全部 smoke |

## 已评估、暂缓或进行中的项

### KASLR（暂缓，前置重构成本大）

内核链接地址固定（如 riscv64 `VIRT_BASE = 0xFFFFFFC080200000`）。
真 KASLR 的障碍不是代码重定位（`-mcmodel=medany` 的文本基本可搬），
而是**直映射偏移 `PAGE_OFFSET` 是编译期常量并在全内核内联使用**
（所有 `pa + PAGE_OFFSET` 形式的直映射访问、外设 MMIO 基址、页表
遍历宏）。可行路线：先把 PAGE_OFFSET 变成 per-boot 运行时变量
（所有引用点收编为 `phys_to_virt()` 类访问器），再在入口汇编按熵源
随机选取偏移并重建引导页表。这是一次横跨 arch/mm/drivers 的大型
重构，收益主要在远程内核信息泄漏威胁模型下；当前威胁模型（研究型
OS、无远程攻击面压力）下暂缓，先完成直映射访问器化作为前置。

### 内核栈 guard page（未做，替代缓解已部分就位）

内核栈是 kmalloc 对象（`kernel/proc/fork.c`），相邻对象间无隔离页。
在 slab 体系下加 guard page 需要分配器支持（对齐翻倍分配 + 底部页
取消映射），成本中等。已有的替代缓解：栈 canary（已落地）、slab
大块尾部哨兵（`kernel/mm/slab.c`）、trap 入口 sp 双界校验（取证硬化
的一部分）。guard page 仍是深度防御缺口，待内存损坏根因收口后评估。

### loongarch64 / ppc64le 的监督模式访问防护（硬件机制缺口）

LoongArch 无 SUM/PAN 等价 CSR 机制；ppc64le 的 KUAP（AMR/IAMR）
需要 key 管理体系，均未实现。两架构当前依赖直映射拷贝集中化 +
软件权限校验，与 SMAP 前的 x86 形态相同。

### user namespace / 完整 capabilities（未做）

无 nsproxy/userns；`CLONE_NEWUSER` 等常量在 clone 白名单中明确拒绝。
capabilities 为 15 个子集（缺 CAP_NET_ADMIN/CAP_BPF/CAP_SYSLOG 等）。

## 熵源说明

`kernel/core/random.c`（xoshiro128+）每 64 次提取混合一次熵源：
timer 节拍、内核地址、空闲帧计数、当前任务指针；x86_64 另有
RDRAND/RDSEED（`arch_hw_entropy_sample()`）。riscv64/aarch64 启动
早期熵质量有限，ASLR/guard 的不可预测性在真实硬件上依赖时钟噪声，
属常见启动期熵不足问题，暂无 virtio-rng 接入。
