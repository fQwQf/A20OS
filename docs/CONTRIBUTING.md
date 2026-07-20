# 贡献指南

本仓库是参赛项目，补丁流程尽量简单直接。下面是贡献者需要遵循的步骤。

## 1. fork 与分支

```bash
git clone <你的 fork>
cd oskernel2025-a20
git checkout -b fix/<简短描述>
# 或 feature/<子系统>-<描述>
```

不要直接在 `main` 分支上提交。

## 2. 本地构建

先确认能编译并运行。详细命令见 [docs/build.md](build.md)。

```bash
make ARCH=riscv64 run
```

## 3. 通过测试门禁

提交前至少运行以下门禁，详细说明见 [docs/testing/testing-gates.md](testing/testing-gates.md)：

```bash
# 构建矩阵
make check-build-matrix

# 基础 bring-up smoke
make smoke-riscv64

# 文档与门禁一致性
make check-doc-test-gates
```

如果修改了具体子系统，再运行对应门禁：

```bash
make check-mm-lock-model        # 内存管理
make check-vfs-abstraction      # 文件系统
make check-abi-boundary         # ABI 边界
make check-driver-core-model    # 驱动
make check-io-progress-model    # I/O 进展
make check-concurrency-foundation # 并发
```

## 4. 提交信息风格

提交信息用于生成历史记录，请保持清晰：

- 标题使用祈使句，例如 `mm: fix COW TLB invalidation on riscv64`。
- 标题不超过 50 个字符。
- 正文说明修改原因和验证方式，可引用 issue。
- 一个提交只做一件事。

示例：

```text
fs: add refcnt helper for vnode lifecycle

Replace direct ref_count manipulation in ramfs/ext4 with
vfile_ref_init / vfile_get / vfile_put_ref_only.

Verified with:
- make check-vfs-abstraction
- make smoke-vfs-stress
```

## 5. Pull Request

1. 将分支推送到你的 fork。
2. 在仓库页面创建 Pull Request。
3. PR 描述中写明：
   - 修改了什么
   - 为什么需要改
   - 运行了哪些门禁
   - 是否关联 issue

CI 通过后，维护者会进行代码审查。请根据评论修改，并确保门禁持续通过。

## 6. 提问

有问题请先查看 [docs/testing/testing-gates.md](testing/testing-gates.md) 和 [docs/build.md](build.md)。若仍未解决，请创建 GitHub Issue，标签选 `question`，并附上相关命令和日志。

##  注意

- 不要直接修改 `docs/testing/testing-gates.md` 或 `docs/project/external-dependencies.md` 中的契约字符串，除非你的改动确实触及这些契约。
- 不要把 `ALLOW_UNVERIFIED_SMP=1` 作为默认门禁参数，除非 PR 专门处理 SMP。
- 提交前请执行 `make clean` 再运行一次关键门禁，避免旧产物造成误判。
