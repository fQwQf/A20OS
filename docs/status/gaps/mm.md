# MM 缺口

对应 V1-MM。现状见 [current/validation-dontunmap.md](../current/validation-dontunmap.md)、
[kernel/mm/fork-mprotect-cow.md](../../kernel/mm/fork-mprotect-cow.md)。

### MM-DOCS-01 RV64 普通镜像构建被未初始化索引诊断阻塞

- 现状：DOCS 包从干净 main 构建 `make -j8 check-doc-test-gates`，`smoke-proc-stress` 的 dev-build 在 `kernel/mm/mm.c` 的 pt_unmap 中被 GCC 的 -Werror=maybe-uninitialized 拒绝，idx_path[level] 可能未初始化；普通镜像和 RAMFS 用户镜像均触发。
- 缺口：包含 runtime smoke 的聚合门禁无法构建；当前不能取得其 guest 运行结论。
- 关闭条件：审查索引路径初始化与遍历范围，修复实际原因，然后以默认警告策略重跑 smoke-proc-stress 与 check-doc-test-gates；不要靠禁用诊断通过。
- 来源：DOCS 包的聚合验收；本包只报告，不修改 MM 源码。
