# 测试与文档门禁

`DOCS_AS_FACT_CONTRACT`：`docs/`、`kernel/abi/*/*.md` 以及顶层状态文件中的架构文档描述当前实现。未来设计应放在规划材料中，而不是事实文档中。

`TEST_FIRST_ARCHITECTURE_MATRIX`：每个架构债务领域在 TODO 条目可以勾选完成前，都必须有一个可重复执行的门禁。

| 领域 | 门禁 |
| --- | --- |
| 并发基础 | `make check-concurrency-foundation` |
| MM/VMA/页表 | `make check-mm-lock-model` |
| I/O 进展 | `make check-io-progress-model` |
| VFS 抽象 | `make check-vfs-abstraction` |
| ABI 边界 | `make check-abi-boundary` |
| 驱动核心 | `make check-driver-core-model` |
| 外部依赖 | `make check-external-dependency-boundary` |

`BUILD_MATRIX_GATE_CONTRACT`：bringup 构建覆盖 `riscv64`、`loongarch64`、`aarch64` 和 `x86_64`；用户态构建通过 `make check-user-build` 覆盖同一组架构。

`ABI_SMOKE_GATE_CONTRACT`：Linux ABI smoke 通过 `smoke-abi-linux` 运行 `syscall_smoke` 和用户态命令；Native ABI 覆盖包括 `native-minimal`、`native-test`、`user/tests/test_liba20c.c`，以及用于 handle dup/transfer 的 `make smoke-native-handle` 运行时覆盖。

`DOC_DRIFT_KEYWORD_GATE`：`stub`、`partial`、`TODO`、`Future`、`not yet`、`for simplicity` 等漂移关键词只有在绑定到明确的覆盖表、TODO 条目或门禁契约时才允许出现。`kernel/external/` 和 `user/external/` 下导入的第三方代码树不参与该门禁。
