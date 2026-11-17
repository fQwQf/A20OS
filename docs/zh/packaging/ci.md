# CI/CD

## 常规 CI

PR、main 提交和手动运行只做三类检查：

1. 基础检查：实例/组件清单、smoke 入口一致性及工具单元测试。
2. 内核编译：由 Makefile 的 `CI_KERNEL_ARCHES` 提供六个 hosted 架构，默认 ABI=both；不为每行追加子系统运行矩阵。
3. 一次 RISC-V64 QEMU 启动：构建内核与用户态，进入 shell，执行外部程序并正常关机（`make smoke-quickstart`）。

构建使用公开的 `ghcr.io/<owner>/a20os-buildenv:latest`，检出固定子模块，复用按架构区分的 ccache。并行构建限制为两个任务；同一 PR/分支的新运行取消旧运行。启动日志在失败时也保存。

Native、SMP、网络、文件系统、集群、MCU 和压力专项继续保留已有 Make 命令，修改相关代码或排查故障时按需执行。它们不再组成自动扩展工作流，也不是每次提交或发布都要重复的固定清单。

```bash
gh workflow run ci.yml --ref <candidate-branch>
# 修改网络代码时，可按需在本地运行：
make smoke-network-suite
```

## 发布

`release.yml` 只负责版本检查、四个发行架构的构建、打包、签名和分发，不再调用整套 CI，也不附加逐镜像启动认证。发布者选择基础 CI 已通过的提交打 tag。

- 手动运行默认制作 base；`devel=true` 另外制作 devel，仅上传 Actions artifact。
- `v*` tag 与源码版本匹配后，制作 base/devel，并发布 GitHub Release。
- 每个架构的 bundle 包含内核、bootstrap FAT、world 镜像、包仓库、公钥及提交/配置说明；附 `SHA256SUMS-<arch>`。
- 仅发布 job 有 `contents: write`；只有四架构构建全部成功才发布。

```bash
gh workflow run release.yml --ref <candidate-branch>
# 确实需要开发环境镜像时：
gh workflow run release.yml --ref <candidate-branch> -f devel=true
```

发布签名使用 secret `A20_REPO_SIGNING_KEY`，公钥自动导出并随 bundle 分发。缺少密钥默认失败；只有仓库显式配置 `ALLOW_UNSIGNED_RELEASE=1` 才允许无签名产物。本地开发密钥不用于正式发布。

## 按需维护

- `quickstart.yml` 只手动运行，用于核对 Ubuntu 上按文档安装依赖的冷启动路径。
- `buildenv.yml` 只手动运行，每次选择一个工具环境 target，默认 `multi`。构建、推送后拉回自检，不自动同时发布八个环境。
- 文档检查按文档路径触发；Pages 继续负责官网部署，与系统发行分开。

工具环境使用 `tools/ci/Dockerfile`；`base` 含宿主工具、Rust、Clang 和镜像工具，各架构 target 加交叉工具链与 QEMU，`multi` 包含全部工具链。更新环境可使用 `tools/ci/build-image.sh <target>` 或手动 buildenv 工作流。公开镜像需要 Packages 的可见性设为 Public，工作流需要 packages 写权限。

## 当前状态

2026-11-17 按项目负责人要求收敛到上述规模，取消原完整 Native/压力矩阵和发布逐镜像认证的强制要求。历史失败记录保留在[候选记录](../status/current/v1.0-candidate.md)，不改写成通过。随后在候选 `94bbdbb12` 上运行一次精简流程，[基础 CI](https://github.com/fQwQf/A20OS/actions/runs/44072919246)的 9 个 job 全部通过：基础检查、六架构内核编译、一次启动及准备步骤。未运行专项矩阵或发布流程；原 N02 已关闭。
