# MM 缺口

对应 V1-MM。现状见 [current/validation-dontunmap.md](../current/validation-dontunmap.md)、
[kernel/mm/fork-mprotect-cow.md](../../kernel/mm/fork-mprotect-cow.md)。

### MM-DOCS-01 RV64 构建阻塞（已关闭）

`pt_unmap` 的 `idx_path` maybe-uninitialized 诊断在 x86 内存路径合并后不再出现；干净 main 上
`make ... dev-build` 以默认警告策略（-Werror）完整通过，`make smoke-proc-stress` 于 2026-10-24
在干净树上 PASS（日志 `.kernel-build/smoke/proc-stress-riscv64.log`，按文档策略不入库）。
关闭条件（修复实际原因 + 默认警告下重跑）已满足，条目移除。
