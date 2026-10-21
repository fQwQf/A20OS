# 文档与门禁自身的缺口

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

### DOCS-04 `check-stm32f103` 在固定 CI 镜像里缺依赖

- 现状：`make check-stm32f103` 需要 `rg`；在 `ghcr.io/fqwqf/a20os-buildenv` 里没有 `ripgrep`，因此该门禁在容器里以 `FileNotFoundError: 'rg'` 退出，而不是给出它本该给的判定。装上 `ripgrep` 后门禁立即给出真实结论（见 [drivers.md](drivers.md) DRIVER-09）。
- 缺口：按 [packaging/ci.md](../../packaging/ci.md) 的说明在构建容器里复现构建/门禁时，这条门禁静默地不可用；它不在 CI 的任何一个 job 里，所以云端也不会发现。
- 关闭条件：镜像带上 `rg`（或该门禁不依赖它），并在一个真实 job 里运行一次。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）。

### DOCS-05 `docs/net/**` 与 `docs/roadmap/**` 的越界行号引用

- 现状：`make check-doc-drift` 最后一步 `tools/check_doc_citations.py` 报 12 条引用越过文件末尾，全部在这两处：
  `docs/net/conntrack-nat.md` 的 `netfilter_nat.c:490-540`、`:520-526`、`:515-519`、`:538`、`:561`、`:554-556`；
  `docs/net/checksum-offload.md` 的两处 `netfilter_nat.c:754-892`（该文件现在只有 503 行，NAT 改写与校验和已拆到 `kernel/net/netfilter_rewrite.c`）；
  `docs/roadmap/lock-serialization-split.md` 的三处 `dcache.c:355-370` / `:357-375`（该文件现在只有 353 行）。
  同一条 make 目标的 `tools/gates.py check-doc-drift` 8 条断言已通过——那些引用落在 DELIVERY 自己的页面上，已修好并重指到当前位置。
- 缺口：这两处文档的行号引用指向的位置不成立，读者按图索骥会落空；它们属于 NET 与路线图文档。
- 关闭条件：这 12 条引用重指到符号当前位置，或对引用而非引文的场合去掉行号，`check_doc_citations.py` 通过。
- 来源：`make check-doc-drift` 输出。
