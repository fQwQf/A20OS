# 文档与门禁自身的缺口

### DOCS-03 用户手册与实现细节的边界

- 现状：`docs/dev/build.md`、`docs/dev/instances.md`、`docs/dev/debug.md` 的用户流程仍包含 ABI 布局、termios 编码和 ptrace 内部状态；`docs/exec-xlator/01-usage.md` 的 W^X 使用说明仍包含内核字段与调用关系。具体窄修请求见 [DELIVERY-DOCS-01](delivery.md) 与 [CORE-DOCS-01](core.md)。
- 缺口：用户流程与技术契约没有完全分开，读者需要筛选。
- 关闭条件：用户操作路径只留用途、前提、命令与失败语义；内部契约在技术文档或源码注释里。
- 来源：[README.md](../../README.md) 文档范围约定。

### DOCS-04 `check-stm32f103` 在固定 CI 镜像里缺依赖

- 现状：仓库的构建镜像依赖与自检已要求 ripgrep，`ci.yml` 的 arm32 kernel-build job 已接入 `make check-stm32f103`。现有固定镜像仍缺 `rg`；本地补齐后门禁可执行，实际停止在 ARMv7-M Rust target 构建断言（[DRIVER-10](drivers.md)）。
- 缺口：更新后的镜像尚未发布，候选的真实云端 job 尚未执行，当前无法确认 CI 获取的新镜像与新接线。这需要交付方按 [packaging/ci.md](../../packaging/ci.md) 发布镜像并执行候选工作流；本包禁止 push，本地复现不计作云端运行。
- 关闭条件：镜像带上 `rg`（或该门禁不依赖它），并在一个真实 job 里运行一次。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）。
