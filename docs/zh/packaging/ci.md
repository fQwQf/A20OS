# CI/CD 详解

## CI/CD 的职责与层次

CI 回答“这次修改是否破坏保留目标的编译和关键行为”；发布验收回答“准备交付的这些文件能否安装、启动和使用”；CD 只分发通过检查的产物。长时压力、性能和特定设备实验保留入口，不作为每次修改的默认全量组合。

| 层次 / 文件 | 触发 | 必须证明的内容 |
| --- | --- | --- |
| 常规 `ci.yml` | PR、main、手动、供 release 调用 | 冷 checkout 的宿主检查；六个 hosted 架构的内核/用户态编译；RISC-V64 Linux/网络/SMP 与 RV64/AArch64 Native 代表性契约；x86 FP 状态回归 |
| 扩展 `ci.yml` | 手动 `extended=true`；正式 tag 发布调用 | 全 Native 套件、额外压力变体、独立集群与 MCU 模拟、Alpine guest 工具链。结果单列；未执行不计覆盖 |
| 候选 `release.yml` | 手动，默认常规检查 + base | 复用同一 CI，四个发行版架构打包、签名、组 base 镜像；直接启动冻结后的内核/FAT/world，确认镜像中程序执行、读写和自然关机 |
| 正式 `release.yml` | tag `v*` | 扩展 CI、base/devel 产物检查均成功后，发布同一批冻结文件；仅发布 job 有 `contents: write` |
| `buildenv.yml` | `tools/ci/**` 变更、手动 | 构建并自检声明的工具环境，不代替 OS 源码/运行验证 |
| `quickstart.yml` | PR、main、手动 | Ubuntu 24.04 按文档安装单架构依赖，从无构建缓存的源码进入 shell 并关机 |

常规 CI 不再同时制作四套 apk/world 后，又在 release 中重复制作。手动派发 release 已包含 CI，无需为同一候选再并行派发一次 CI。共享入口使用 [GitHub reusable workflow](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows)，产物和证据按 [workflow artifacts](https://docs.github.com/en/actions/concepts/workflows-and-actions/workflow-artifacts) 保存。

默认层的跳过项包括扩展集群、额外压力、Alpine 工具链和 AArch64 扩展压力；手动候选还跳过 devel 和 tag 专属发布。这些是未运行的范围，不是成功结果。实际错误、超时、依赖失败导致的跳过必须分别报告，不能靠调整分层把已发现的产品缺陷写成通过。

2026-11-17 的第一轮真实云端 [CI](https://github.com/fQwQf/A20OS/actions/runs/44034951962) 和 [release](https://github.com/fQwQf/A20OS/actions/runs/44034957500) 对应 `1b667fe8e`，均失败。它们发现了子模块检出、BSD Clang 依赖、宿主工具和本机私有脚本假设等阻塞；原结果保留，不作为新分层的通过证据。最新状态见[候选记录](../status/current/v1.0-candidate.md)。

## 构建容器（环境可复现的关键）

镜像分两层：一个与架构无关的 `base`，和每个架构一个 target。`base` 装`debian:trixie-slim` + 镜像工具（mtools / dosfstools / e2fsprogs）、`fakeroot`、`ccache`、`openssl`、Python 3、宿主 gcc、BSD 后端需要的 `clang-19`，以及 **Rust**（`rustc`
+ `cargo` + `rust-src` + 宿主 std）。每个架构 target 只加自己的交叉 gcc、它的 `libc6-dev-*-cross`、对应的 QEMU 和它自己的 no_std Rust target。

| target | 额外内容 | 约 |
|---|---|---|
| `base` | — | 1.7 GB |
| `riscv` | `riscv64-unknown-elf`（含 rv32 multilib）、`riscv64-linux-gnu`、`qemu-system-riscv*`、rust `riscv64imac/riscv64gc-unknown-none-elf` | 2.7 GB |
| `aarch64` | `aarch64-linux-gnu`、`qemu-system-arm`、rust `aarch64-unknown-none` | 2.0 GB |
| `x86_64` | `qemu-system-x86`、rust `x86_64-unknown-none`（gcc 用 base 的宿主） | 1.9 GB |
| `loongarch64` | `loongarch64-linux-gnu`、`qemu-system-misc`、rust `loongarch64-unknown-none` | 2.3 GB |
| `arm32` | `arm-linux-gnueabihf`、`qemu-system-arm`、rust `armv7a-none-eabihf` | 2.0 GB |
| `armv7m` | `arm-none-eabi`、`qemu-system-arm`、rust `thumbv7m-none-eabi` | 2.4 GB |
| `multi` | 以上全部；发布为 `:latest` | 4.5 GB |

（体积是本机实测的安装体积，随 Debian 快照浮动。`multi` 比旧的单镜像 3.4 GB 更大，因为它补进了原先完全缺失的 Rust；单架构镜像因此比 `multi` 小 1.8–2.6 GB，这才是按架构拉取省下的量。）

为什么要拆：单镜像要求每个 job 拉下**所有**架构的工具链；更糟的是缺包是隐形的，只有真去编那个架构才暴露。这个镜像正是这样同时缺过两样东西——每个容器内内核构建都要的 Rust 一点没有，armhf 的 libc 开发头也没有（交叉 gcc 只把`libc6-dev-*-cross` 列为 **Recommends**，而 Dockerfile 用`--no-install-recommends`，于是编译器在、`#include <limits.h>` 就死）。两处都记账：前者让 `kernel-build-*` 永远绿不了，后者是 arm32 报`bits/libc-header-start.h` 的直接原因。

`riscv64` 与 `riscv32` 共用一个 `riscv` 镜像：rv32 内核就是同一个`riscv64-unknown-elf` 的 rv32 multilib，rv32 的 Rust core 由`-Z build-std=core` 从 `rust-src` 现场编，所以没有对应的 rustup target。

### 每个 target 都要自检

`tools/ci/image-selftest.sh <target>` 只回答一个问题：**这个镜像声称的工具链在不在**。它为每个 target 编一个 freestanding 对象、预处理一次 libc 头、为每个声明的 Rust target 编一个 no_std crate（`riscv` 还会真的跑一次`-Z build-std=core`，因为那是最容易静默坏掉的一步），并检查每个 QEMU 能起来。

自检现在也检查 `clang-19` 并实际编译一个 freestanding 对象。它**刻意不跑** `check-<arch>-bringup`。那条门禁回答的是"这棵树能不能编"，是源码问题；把两者混在一起，镜像缺件会被误读成源码坏了，源码坏了又会让镜像看起来缺件。

### 本地构建镜像

Docker 只是辅助——用宿主自己的工具链（[../dev/build.md](../dev/build.md)）不需要它。要复现 CI 环境或改镜像时：

```bash
tools/ci/build-image.sh riscv        # 构建 + 自检一个 target
tools/ci/build-image.sh all          # 全部，multi 在最后
```

也可以在同一个镜像里手工复现：

```bash
docker run --rm -it -v "$PWD:/src" -w /src a20os-buildenv:riscv bash
# 容器内：make ARCH=riscv64 dev-build
```

### CI 里怎么用

`buildenv.yml` 发布后会拉取公开镜像自检；日常 job 使用 resolver 给出的镜像，并在日志记录真实拉取的 digest 和工具版本。旧镜像尚未包含新声明的 Clang 依赖时，版本化的 `build-setup` action 在 job 内安装 `clang-19`；这不等于旧 registry tag 已更新。Dockerfile 和镜像自检已声明该依赖，后续构建镜像时会直接携带它。

每个构建都检出固定子模块。宿主检查使用稳定的 Ubuntu 24.04，并显式安装 Markdown、MkDocs、FAT 工具和所用的 RISC-V 交叉编译器；测试不依赖开发机 `.git` 里的私有脚本。运行镜像仍可按已有架构 tag 选择，当前多架构 job 使用 multi。

### 首次启用 / 更新镜像

`buildenv.yml` 需要推 ghcr，首次使用前：

1. 仓库 Settings → Actions → General → Workflow permissions 设为"Read and write permissions"（GITHUB_TOKEN 才能推 packages）；
2. 手动触发一次 buildenv（Actions → buildenv → Run workflow）；
3. 到 Packages 页面把 `a20os-buildenv` 设为 Public（fork 的 PR 才能免认证拉取）。

### ccache

CI 用 `actions/cache` 缓存 `build/cache/ccache`（按架构分 key，`restore-keys` 前缀回退）。本地容器构建想复用同一套缓存，把目录挂进去即可：`-v $PWD/build/cache/ccache:/src/build/cache/ccache`。

## ci.yml 的工作分解

1. resolver 从 Makefile 读取 `CI_KERNEL_ARCHES`，保留六架构唯一来源；另解析小写构建镜像名。
2. 宿主 job 先检查实例、工具和公共行为。它失败时，不启动后续重量级构建/运行。
3. 六个架构各执行 `check-<arch>-bringup` 和 `check-user-build-stamp`；ARM32 行另跑 STM32 固件门禁，x86 行另跑已有 FP 状态回归。内核/用户态构建不是该架构的所有运行行为认证。
4. RV64 运行 Linux ABI、调度/进程 SMP2、futex SMP4、MM、VFS SMP2 和两项网络检查；RV64/AArch64 各运行 Native contract、deepen、MM、IPC。不要求每个架构重复全部负载。
5. 仅扩展层再运行完整 Native 套件、额外单核/swap 变体、AArch64 压力、集群模拟与 guest 工具链。TCG 下的结果不外推到实板。

源码构建缓存用于加速，不能代替冷检出的依赖准备。CI 使用并发组淘汰同一分支的过时检查；发布 job 不放进该组。运行日志即使失败也上传，job 与步骤的 skipped 分别分类。

可单独运行常规或扩展检查：

```bash
gh workflow run ci.yml --ref <candidate-branch>
gh workflow run ci.yml --ref <candidate-branch> -f extended=true
```

## release.yml 的工作分解

1. 检查源码版本，tag 还须匹配版本号；调用上述 CI，正式 tag 使用扩展层。
2. CI 成功后，四个发行版架构构建内核/用户态、bootstrap FAT、签名仓库和 base world；tag 或显式扩展模式再制作 devel。保留正式发布的四架构和 base/devel 能力。
3. 把最终内核、FAT、world、公钥和仓库复制到本次交付目录。此后不再构建这些文件。
4. `check_release_image.py` 直接启动这些文件：base 在 `/extra` 执行镜像内 BusyBox 并复制/比较/删除镜像内文件；devel 要进入 Alpine 根。两者还检查普通命令、文件操作、sync、正常退出。QEMU snapshot 写入临时覆盖层，检查前后输入散列必须相同；本条不是断电持久化验收。
5. 只对通过检查的文件制作各架构 bundle，附源码提交、配置、`SHA256SUMS-<arch>` 和串口报告。bundle 含内核与 bootstrap，不再只给出缺少启动配套的孤立 world。
6. 只有 tag 且全部依赖成功才创建 GitHub Release；发布的是检查过的 bundle 与镜像，不重新编译。候选分支运行不创建正式 Release。

手动候选验收只需派发这一条：

```bash
gh workflow run release.yml --ref <candidate-branch>
# 若需要完整 tag 前演练：
gh workflow run release.yml --ref <candidate-branch> -f extended=true
```

原始 Actions 结果、确切提交、每个 job 的结论、跳过原因、实际工具/镜像身份和产物散列共同构成证据。工作流文件存在、通过 YAML 检查或本地定向测试，都不等于云端候选已经通过。

### 发布准备（一次性）

1. **发布密钥**（默认必需）：
   ```bash
   openssl genrsa -out a20os-release.rsa 4096
   openssl rsa -in a20os-release.rsa -pubout -out a20os-release.rsa.pub
   ```
   私钥内容粘进 Settings → Secrets → Actions 的`A20_REPO_SIGNING_KEY`；公钥由 workflow 从私钥导出并随 artifact 上传，无需入库。缺少该 secret 时发布默认失败。只有管理员明确将 repository variable `ALLOW_UNSIGNED_RELEASE` 设为 `1`，才允许无签名发布；这会使产物无法通过仓库签名验证，不应作为 CI 修复手段；
2. 打注解 tag（tag 正文会成为 Release 的发布说明）：内核版本与候选提交通过验证后，例如 `git tag -a v0.17 -m "..." && git push origin v0.17`。

Pages（Settings → Pages → Source 选 "GitHub Actions"）只服务于 `pages.yml`的官网站点，与 release 无关。

## 常见故障

| 现象 | 原因与处理 |
|------|-----------|
| `UNTRUSTED signature` | 消费时缺 `--keys-dir` 或公钥名与打包时 `--key-name` 不一致 |
| `no such package` 且仓库明明有 | 索引签名不被信任会**静默丢弃整个仓库**。检查 keys-dir 是否传了**绝对路径**（apk 对相对 keys-dir 会因内部 chdir 而失效，mkrootfs 已代为绝对化，手工调用 apk 时注意） |
| `unexpected end of file`（读包时） | 包不是 mka20pkg 产物：apk v2 的分段 tar 格式约束见 [apk-format.md](apk-format.md) |
| 容器里 `git` 报 dubious ownership | 加 `git config --global --add safe.directory "$GITHUB_WORKSPACE"`（workflow 已含） |
| loongarch64 工具链缺失 | 容器默认装 `gcc-loongarch64-linux-gnu`（Debian cross-ports）；个别快照期缺失时按 docs/zh/dev/build.md 用 Loongson 官方工具链 |
