# RUNTIME 缺口

对应 V1-RUNTIME。现状见 [current/native-abi-runtime-status.md](../current/native-abi-runtime-status.md)。

### RUNTIME-01 完整 libc 与动态链接

- 现状：mlibc 的最小运行时、动态 sysroot 与部分 mlibc smoke 可用。
- 缺口：完整 libc 行为、`ld.so`/`dlopen`、TLS 多线程与库调用没有端到端验证；不得用 fake `ld.so`。
- 关闭条件：真实 `ld.so`/`libc.so` 启动真实动态 Native 程序，覆盖 TLS 多线程、库调用、dlopen、退出与 re-exec。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) 优先级表第 10 项。

### RUNTIME-02 exec 继承

- 现状：exec 会保留部分 fd 与线程语义；继承矩阵不完整。
- 缺口：fd、线程、信号处置、`$ORIGIN`/符号链接路径在 exec 后的行为未逐项验收。
- 关闭条件：每个继承项都有 guest 用例，成功与拒绝路径都明确。
- 来源：原 `known-issues.md` 当前工程队列。

### RUNTIME-03 Java 工作负载

- 现状：JVM 能启动，部分应用可运行；退出异常与 GC/JIT 行为有历史问题。
- 缺口：没有端到端的 Java/JIT/GC 工作负载与清理契约。
- 关闭条件：固定 Java 程序完成启动、工作、退出与资源回收，冷启动与峰值内存可复现。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) V1-RUNTIME 行。

### RUNTIME-04 kexec handoff

- 现状：kexec staging 与事务路径有实现；被测 QEMU 路径上 handoff 不可用。
- 缺口：没有真实 machine handoff 的运行证据，staging 不能当 handoff。
- 关闭条件：在可用平台上完成一次真实 handoff 并自然启动；否则明确记录为平台限制。
- 来源：[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。

### RUNTIME-05 NOMMU 与平台矩阵

- 现状：NOMMU 只有对象构建，不是 guest 验证。
- 缺口：NOMMU/MCU profile 的运行时行为与保留平台矩阵没有覆盖。
- 关闭条件：适用平台完成 guest 运行；不适用项明确标为 N/A 而不是 PASS。
- 来源：[current/module-status-2026-10-19.md](../current/module-status-2026-10-19.md)。
