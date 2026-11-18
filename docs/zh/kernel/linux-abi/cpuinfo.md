# `/proc/cpuinfo` 多核处理器信息上报规范

A20OS 在 `/proc/cpuinfo` 中根据系统在线 CPU 掩码（Online CPU Mask）为每个逻辑核心生成独立的 processor 条目，确保多核环境下的运行时环境（如 JVM、OpenMP、多线程渲染引擎）能够正确识别系统并行计算能力。

---

## 1. 跨架构格式规范

内核在生成 `/proc/cpuinfo` 内容时遵循以下规范：
- **按核遍历输出**：对系统当前在线 CPU 掩码做原子快照，为每个在线核心格式化输出独立的 `processor : N` 记录。
- **架构特定字段扩展**：各架构通过 `arch_cpuinfo_fields()` 注入原生特征。例如 x86_64 在 `kernel/arch/x86_64/platform/arch_hooks.c` 中通过 `CPUID` 指令动态读取 vendor ID 与 model name 字符串；RISC-V 则报告 ISA 扩展与 MMU 模式。
- **回退兜底**：当硬件探测未识别出详细信息时，只上报基准架构名称，避免捏造不存在的硬件拓扑或特性位。
- **动态缓冲区与大小契约**：打开该 proc 虚拟文件时，输出缓冲区根据内核配置的最大 CPU 容量自动伸缩；由于内容为动态生成，文件大小上报为 0。

---

## 2. 自动化验证

- **单元测试**：`cpuinfo_test` 验证每个 processor ID 的唯一性，核对每个 ID 与调用线程亲和性（affinity）的匹配度，并校验核心总数与硬件配置一致。
- **门禁验证**：
  - `make smoke-cpuinfo`：在单核与多核 RISC-V 环境中验证基础解析。
  - SMP 门禁：在四核 x86_64 与 RISC-V 实例下与 `sigchld_group_test` 并行回归，确保无并发快照竞争或缓冲区截断。
