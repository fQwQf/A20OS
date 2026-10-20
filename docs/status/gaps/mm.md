# MM 缺口

对应 V1-MM。现状见 [current/validation-dontunmap.md](../current/validation-dontunmap.md)。

### MM-01 DONTUNMAP 的换出源与共享文件路径

- 现状：私有匿名页的目标保留、源 refault、fork/COW、RSS 清理与确定性回滚已覆盖。
- 缺口：源 PTE 已换出（swap-in）时槽/引用保留与源 refault 未验证；共享文件失败路径、更多回滚/分配器阶段、并发变更未覆盖。
- 关闭条件：确定性 swap-in guest 用例同时核对源/目标字节与换入换出计数；共享文件与并发变更用例通过。
- 来源：requirements N09、[current/validation-dontunmap.md](../current/validation-dontunmap.md)。

### MM-02 活 PTE 的 TLB 拆除

- 现状：unmap 路径有 TLB 完成后再释放的顺序约束；活 PTE 的拆表压力未测。
- 缺口：映射页在并发访问下的 TLB teardown 与释放时序没有对应门禁。
- 关闭条件：并发访问 + unmap/reclaim 的 SMP 用例通过，释放后不再命中；负控能复现旧错误。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) V1-MM 行。

### MM-03 共享写的冷启动与磁盘一致性

- 现状：共享文件写入在 `mprotect(PROT_READ)`/unmap 时不丢 dirty 的路径已修，RV64 有字节精确冷启动读回与 e2fsck 证据。
- 缺口：其他架构与更多映射形态没有同等证据；dirty-state 跨 shutdown 的完整矩阵未覆盖。
- 关闭条件：适用架构上共享写 → 只读保护 → 关机 → 冷启动读回字节一致且 e2fsck 干净。
- 来源：[kernel/mm/fork-mprotect-cow.md](../../kernel/mm/fork-mprotect-cow.md)。

### MM-04 分配回滚与 OOM 路径

- 现状：部分分配失败有逐阶段回滚；OOM 与分配失败的完整回滚矩阵未覆盖。
- 缺口：rollback-leaf/split-fail 之外的失败阶段（页表分裂、大页、swap 槽）没有系统性注入。
- 关闭条件：每个失败阶段都有确定性注入，且资源计数回到操作前的水平。
- 来源：[current/v1.0-progress.md](../current/v1.0-progress.md) V1-MM 行。
