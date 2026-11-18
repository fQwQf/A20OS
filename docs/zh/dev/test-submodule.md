# 可选测试子仓库

主仓负责操作系统实现、构建、镜像与运行工具；集成测试放在独立的 [a20-tests](https://github.com/fQwQf/a20-tests) 仓库，并以 `tests/` submodule 固定版本。

## 日常构建和轻量测试

```sh
make ARCH=riscv64          # 等同 kernel-only，不运行测试
make test                 # 5 个宿主 C 单测和 ELF 解析测试
make host-tests           # make test 的兼容入口
make check                # 清单检查和轻量测试
make check-docs           # 文档一致性
make release-rv           # 显式的 RISC-V 发布构建
make release-la           # 显式的 LoongArch 发布构建
```

这些命令不需要初始化测试子仓库。轻量测试使用宿主 C 编译器与 Python 标准库，不启动 QEMU，也不调用 Rust 或交叉工具链。镜像与内核构建仍需要对应的构建依赖。

## 导入集成测试

```sh
git submodule update --init tests
make tests-help
make check-smoke-cases
make smoke-quickstart
make -C tests smoke-quickstart
```

导入后无需复制脚本或创建兼容软链接。只有显式请求集成测试目标时才加载测试 Make 片段；子仓的存在不改变 `make test` 或 `make check` 的依赖。具体用例需要的 QEMU、交叉编译器、Rust target 和镜像工具仍须安装。

原来的完整宿主测试聚合改名为 `make full-host-tests`，完整宿主门禁聚合改名为 `make check-integration-host`。`make check-a20-tests` 和 `make smoke-*` 在导入后继续可用。

## CI 分层

PR 验证内核构建矩阵与主仓轻量检查。定时或手工集成工作流导入固定版本的子仓并运行 `make tests-nightly`。发布验收使用 `make tests-release`；通过 `RELEASE_TEST_TARGETS` 指定实际需要的验收目标。默认验收覆盖代表性的启动与 ABI，不代表所有压力、集群、GPU 或物理硬件已通过。

这次迁移保留既有用例语义。旧用例仍可能读取源码、构建专用镜像、依赖串口标记和内核测试开关；源码无关的工件测试接口及测试钩子的进一步收敛属于后续工作，不能仅凭移动文件宣称已经实现。

## 历史裁剪和 tag

```sh
python3 tools/repo/prepare-test-history.py \
  --output /absolute/path/outside-the-checkout
```

该命令需要 `git-filter-repo`，输出目录必须不存在且位于当前仓库之外。它备份有引用的历史，在新镜像中剔除迁出路径及识别出的旧路径，分别生成主仓和测试仓历史；不会改写当前工作区、索引或源仓引用，也不会推送远端。镜像只包含已经提交的历史，发布前必须导入并审阅当前工作区中的迁移改动。

所有主仓分支与 tag 一起改写。保留空 commit，避免仅修改测试的带 tag commit 被自动折叠。检查轻量/附注 tag 的类型、每个旧 commit 到新 commit 的映射，以及各 tag 下除删除路径外的树内容不变；随后扫描全部历史路径并运行 `git fsck`。已签名的附注 tag 在改写后需要重新签名，报告会列出对应名称。

最终发布需协调所有远端分支与 tag，避免遗漏的旧引用继续保留被删对象；不要直接 `push --mirror` 发布本地备份分支或工具内部引用。GitHub 的 PR 引用、缓存和旧克隆可能仍持有旧对象，不能用一次强推保证服务端或其他克隆中的物理删除。旧克隆的 reflog、备份引用和子模块对象库也不会因新历史自动缩小。历史备份应留在主仓之外。
