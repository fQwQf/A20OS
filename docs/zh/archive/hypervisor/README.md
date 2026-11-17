# Hypervisor 历史验证数据归档 (Hypervisor Archive)

本目录归档 A20OS 虚拟化与 Hypervisor 子系统在多架构后端验证、SMP 验证和虚拟 SBI 研发阶段所生成的历史测试清单与输出日志。

---

## 归档文件清单

| 归档文件 | 阶段与内容说明 |
| :--- | :--- |
| [validation-2026-10-25.json](validation-2026-10-25.json) | 2026-10-25 多架构（RV64/RV32/ARM32/AArch64/AMD SVM/Intel VMX）双 vCPU SMP 运行验证结果清单与日志哈希 |
| [validation-main-2026-10-25.json](validation-main-2026-10-25.json) | 主分支合入后的 14 份运行记录、镜像与日志哈希清单 |
| [validation-sbi-2026-10-27.json](validation-sbi-2026-10-27.json) | 2026-10-27 RISC-V 虚拟 SBI 规范正反向测试清单与日志路径记录 |
| `validation/` | 详细的硬件与 QEMU 运行控制台原始输出日志目录 |
