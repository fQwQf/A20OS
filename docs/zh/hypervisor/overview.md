# A20OS 虚拟化与 Hypervisor 子系统架构规范

A20OS Hypervisor 是内核能力（Capability）体系下的轻量级硬件虚拟化地基。它不追求对庞杂宿主驱动模型（如全功能 KVM）的逐条复刻，而是将虚拟机（VM）与虚拟处理器（vCPU）抽象为原生的内核资源对象，深度复用内核的单级内存模型与统一调度框架。

代码位置：[`kernel/hyp/`](../../../kernel/hyp/)、[`kernel/include/hyp/`](../../../kernel/include/hyp/)、[`kernel/arch/*/hyp/`](../../../kernel/arch/)。

---

## 1. 核心架构与设计原则

```
+-------------------------------------------------------------+
|                      用户态 VMM / 管理工具                    |
|                hyp_boot / hypvm / hyp_guest                 |
+-------------------------------------------------------------+
                              | 系统调用: sys_native_hyp
                              v
+-------------------------------------------------------------+
|                   内核 Hypervisor 核心抽象                    |
|       hyp_vm_t (地址空间、设备模型) / hyp_vcpu_t (执行线程)     |
+-------------------------------------------------------------+
           |                                       |
           v                                       v
+-----------------------+              +-----------------------+
|  Stage-2 二阶段内存    |              |  体系结构硬件后端     |
|  复用 MM_AS_MODEL     |              |  RV64/RV32 H, ARM HYP |
|  FRAME_F_GUEST 帧出借  |              |  AArch64 EL2, VMX/SVM |
+-----------------------+              +-----------------------+
```

### 1.1 Stage-2 地址空间复用单级模型

Hypervisor 的客户机物理地址空间（GPA -> HPA）直接作为内核单级内存模型（`MM_AS_MODEL`）的一个特殊实例：
- **同型 Radix 树**：Stage-2 页表页同样挂载 `pt_meta_t`，节点级 MCS 队列锁与前序加锁/逆序解锁契约完全通用；
- **状态字节分类**：Stage-2 映射叶项标记为专有的 `MM_ST_GUEST_MEM` 类别，明确区分于宿主进程的匿名页与文件页；
- **页帧出借语义（Frame Lending）**：
  - 客户机 RAM 由宿主内核的物理页帧出借（`mm_pt_frame_lend()`）提供；
  - 物理帧打上 `FRAME_F_GUEST` 标志并自增引用计数；
  - VM 销毁时通过 `mm_pt_frame_return()` 归还；系统退出时调用 `mm_s2_audit()` 审计所有 Stage-2 映射与页帧标志的一致性，防止内存泄露。

### 1.2 线程化 vCPU 与轻量调度

- **独立执行载体**：每个 vCPU 对应一个内核调度实体（`task_t`），复用通用进程调度器（EEVDF）进行时间片分配；
- **每 CPU 活跃槽位**：多核 SMP 采用 per-CPU 活跃槽位注册，彻底消除了早期的全局单槽串行竞争；
- **Trap 分发与退出处理**：硬件退出事件（VM-Exit / Traps）由架构专属汇编汇聚后，解包为标准退出原因（如 MMIO 访存、SBI/Hypercall 调用、定时器到期、关机指令），返回通用层处理。

---

## 2. 硬件后端支持矩阵

A20OS Hypervisor 抽象层（`hyp_backend_t`）已对接主流硬件架构的硬件辅助虚拟化扩展：

| 架构 | 硬件扩展标准 | 状态 | 关键实现细节 |
|:---|:---|:---|:---|
| **RISC-V 64** | Privilege H 扩展 (v1.0) | **完整可用** | 支持 VS/VU 模式运行、虚拟 SBI、两阶段 Sv39x4 分页、多核 SMP |
| **RISC-V 32** | Privilege H 扩展 | **可用** | 支持 RV32 H 虚拟化扩展、Sv32x4 分页、32 位虚拟 SBI 接口 |
| **AArch64** | ARMv8.1-A VHE / EL2 | **可用** | 基于 Stage-2 转换表、SPSR_EL2 状态管理、双 vCPU 验证 |
| **ARM32** | ARMv7-A Virtualization (HYP) | **可用** | 基于 Cortex-A15 / A7 硬件虚拟化扩展、LPAE 描述符格式 |
| **x86-64 (Intel)** | Intel VT-x (VMX) | **可用** | 完整 VMCS 配置、EPT 二级页表翻译、Bochs / QEMU 双核验证 |
| **x86-64 (AMD)** | AMD-V (SVM) | **可用** | VMCB 控制块抽象、NPT 二级页表支持、双核 SMP 验证 |
| **LoongArch** | LVZ 虚拟化扩展 | **实验性** | 基础 CSR 与结构体抽象，默认关闭，待物理机正控验证 |

---

## 3. RISC-V 虚拟 SBI 规范实现

在 RISC-V 架构下，Hypervisor 内核直接充当客操作系统的 Supervisor Binary Interface (SBI) 提供者，支持标准 SBI 扩展：

1. **基础扩展 (Base, EID 0x10)**：实现规约探测（`sbi_probe_extension`）、规格版本查询（返回 SBI 2.0）；
2. **调试控制台扩展 (DBCN, EID 0x4442434E)**：接管客户机早期控制台输出（`sbi_debug_console_write` / `read`），零拷贝透传至宿主控制台；
3. **处理器状态管理 (HSM, EID 0x48534D)**：支持客户机多核启动（`sbi_hart_start`）与状态查询（`sbi_hart_get_status`）；
4. **远程屏障与 IPI (RFENCE 0x52464E43, IPI 0x735049)**：模拟跨核中断注入与页表刷新通知；
5. **定时器扩展 (TIME 0x54494D45)**：模拟客户机 `stimecmp` / `vstimecmp` 定时器下发。

---

## 4. 设备模型与客户机交互

A20OS 采用内核内极简设备模型，重点保证轻量与可靠：

- **UART 16550A 模拟**：
  - 位于 GPA `0x10000000`，提供 8 字节寄存器窗口（RBR/THR/IER/IIR/LCR/LSR/MSR/SCR）；
  - 采用无阻塞环形队列（Ring Buffer）实现双向控制台字符搬运。
- **CLINT 核心局部中断器**：
  - 位于 GPA `0x02000000`，模拟 `msip` 与 `mtimecmp` 寄存器映射，支持软件中断与时钟中断注入。
- **用户态交互流**：
  - 宿主运行 `hyp_boot` 装载内核镜像与设备树（FDT）；
  - 客户机启动后加载 A20OS RAMFS 用户态程序（mksh、基础核心工具）；
  - 客户机执行 `exit` 退出 shell 触发关机序列，宿主收到 `HYP_EXIT_SHUTDOWN` 完成资源优雅释放。

---

## 5. 验证门禁与回归方法

Hypervisor 的稳定性由自动化测试套件直接保障：

| 门禁名称 | 目标平台 | 验收标准 |
|:---|:---|:---|
| `smoke-hyp-a20os` | RISC-V 64 | 验证 A20OS 内核作为 Guest 成功启动并完成早期自检 |
| `smoke-hyp-shell` | RISC-V 64 | 验证真 RAMFS 用户态 Guest 启动进入 mksh，交互执行测试指令并正常退出 |
| `smoke-hyp-sbi` | RISC-V 64 / 32 | 验证虚拟 SBI 规范全部 40 项正反向断言与多核 HSM 启动 |
| `smoke-hyp-smp` | 多架构 | 验证各架构后端双 vCPU SMP 并发调度与 IPI 通信 |
| `smoke-hyp-vmx` | x86-64 | 基于 Bochs 验证 Intel VMX 硬件虚拟化扩展初始化与执行 |
