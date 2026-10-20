# 文档与门禁自身的缺口

### DOCS-01 行号引用漂移

- 现状：`make check-doc-drift` 由四步组成，其中两步仍为红。
  `tools/gates.py check-doc-drift` 的 8 条断言**已通过**——DELIVERY 修掉了落在
  `docs/testing-gates.md`、`docs/status/current/server-readiness.md`、
  `docs/status/history/mm-single-level-migration-log.md` 与 `kernel/external/lwip/DIVERGENCE.md`
  里指向已不存在位置的行号引用，并重新生成了 v1 requirements 清单。
  仍然失败的是同一条 make 目标里的另外两步：
  `tools/check_doc_citations.py` 报 12 条引用越过文件末尾，集中在 `docs/net/checksum-offload.md`、
  `docs/net/conntrack-nat.md`（`netfilter_nat.c:490-540`、`:520-526`、`:515-519`、`:538`、`:561`、
  `:554-556`、`:754-892` 等，该文件现在只有 503 行）与 `docs/roadmap/lock-serialization-split.md`
  （`dcache.c:355-370` 等，该文件现在只有 353 行）；
  `tools/gen_linux_syscall_coverage.py --check` 因 [ABI-08](abi-ipc.md) 的未登记符号失败。
- 缺口：`docs/net/**` 与 `docs/roadmap/**` 里的行号引用指向的位置已不成立，读者按图索骥会落空；
  这两处属于 NET 与路线图文档，DELIVERY 未替它们判定。
- 关闭条件：`check_doc_citations.py` 的 12 条引用重指或去掉行号，且 ABI-08 关闭后
  `check-doc-drift` 整条目标通过。
- 来源：`make check-doc-drift` 输出。

### DOCS-04 `check-stm32f103` 在固定 CI 镜像里缺依赖

- 现状：`make check-stm32f103` 需要 `rg`；在 `ghcr.io/fqwqf/a20os-buildenv` 里没有 `ripgrep`，因此该门禁在容器里以 `FileNotFoundError: 'rg'` 退出，而不是给出它本该给的判定。装上 `ripgrep` 后门禁立即给出真实结论（见 [drivers.md](drivers.md) DRIVER-09）。
- 缺口：按 [packaging/ci.md](../../packaging/ci.md) 的说明在构建容器里复现构建/门禁时，这条门禁静默地不可用；它不在 CI 的任何一个 job 里，所以云端也不会发现。
- 关闭条件：镜像带上 `rg`（或该门禁不依赖它），并在一个真实 job 里运行一次。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）。

### DOCS-02 冻结审计仍被门禁引用

- 现状：`docs/archive/` 下 6 份审计仍被 `tools/gates.toml` 作为契约标记的输入文件。
- 缺口：它们的结论已过时，但删除需要同时改门禁断言。
- 关闭条件：决定保留（明确其冻结语义）或删除（同步改门禁），二者择一。
- 来源：[archive/README.md](../../archive/README.md)、`tools/gates.toml`。

### DOCS-03 用户手册与实现细节的边界

- 现状：部分参考页仍包含实现内部细节（锁序、ownership、内部结构）。
- 缺口：用户流程与技术契约没有完全分开，读者需要筛选。
- 关闭条件：用户操作路径只留用途、前提、命令与失败语义；内部契约在技术文档或源码注释里。
- 来源：[README.md](../../README.md) 文档范围约定。
