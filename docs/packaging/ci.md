# CI/CD 详解

## 构建与发布 workflow

| 文件 | 触发 | 职责 |
|------|------|------|
| `buildenv.yml` | `tools/ci/**` 变更 / 手动 | 按架构构建构建容器镜像并自检，推 `ghcr.io/<owner小写>/a20os-buildenv:<target>`，`multi` 另发 `:latest` |
| `ci.yml` | PR / push main / 手动 | 宿主侧源码契约门禁（快速）→ 逐架构内核 bring-up 构建矩阵 → 四架构打包建库 + 组 base 镜像 → 独立 smoke job 跑 riscv64 全套 QEMU 冒烟 → 上传 artifact |
| `release.yml` | tag `v*` / 手动 | 全架构发布构建 + base/devel 镜像 → 发布密钥签名 → GitHub Release（附件 = 各架构镜像） |
| `quickstart.yml` | PR / push main / 手动 | Ubuntu 24.04 从干净 checkout 安装单架构依赖与固定 Rust，环境检查 → 完整开发镜像 → shell / 外部命令 / 正常关机；不依赖 GHCR 构建镜像或编译缓存 |

首次体验优先按 [构建指南](../dev/build.md) 操作。`quickstart.yml` 与综合门禁独立，可以直接观察新用户路径是否成功；新增 workflow 的定义本身不代表远端已运行通过。

镜像名小写：ghcr 镜像名必须全小写，而 `github.repository_owner` 可能含大写（如 `fQwQf`，buildx 会直接报 `repository name must be lowercase`）。三个 workflow 都先经一个 `buildenv-image` 解析 job 用 shell 小写化 owner，再在`container.image` / build-push `tags` 里引用结果；`container.image` 在 job 启动前解析、用不了步骤内 shell，所以必须走独立的 resolver job。

## 构建容器（环境可复现的关键）

镜像分两层：一个与架构无关的 `base`，和每个架构一个 target。`base` 装`debian:trixie-slim` + 镜像工具（mtools / dosfstools / e2fsprogs）、`fakeroot`、`ccache`、`openssl`、Python 3、宿主 gcc，以及 **Rust**（`rustc`
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

它**刻意不跑** `check-<arch>-bringup`。那条门禁回答的是"这棵树能不能编"，是源码问题；把两者混在一起，镜像缺件会被误读成源码坏了，源码坏了又会让镜像看起来缺件。

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

`buildenv.yml` 对每个 target 构建、推送、再把**发布出去的镜像**拉回来跑自检，所以绿的断言覆盖的是别人实际拉到的东西。`:latest` 仍指向 `multi`，`ci.yml`里未改动的 job 照旧工作。

按架构切换容器是**可选的后续项**，需要先跑过一次 `buildenv.yml` 让各 tag 存在。改法只有两处：在 `ci.yml` 的 `buildenv-image` resolver 里多输出`image_riscv`/`image_aarch64`/…（`riscv32` 映射到 `riscv`），再把`kernel-build` matrix job 的 `container.image` 指向对应 arch 的 tag。其余 job（打包、smoke、cluster）保留 `:latest`：`check-kernel-build-all`、`check-user-build-all` 与 `tools/check-cluster-instance.py --arches riscv64,aarch64,x86_64` 本来就需要多架构同时在手。

### 首次启用 / 更新镜像

`buildenv.yml` 需要推 ghcr，首次使用前：

1. 仓库 Settings → Actions → General → Workflow permissions 设为"Read and write permissions"（GITHUB_TOKEN 才能推 packages）；
2. 手动触发一次 buildenv（Actions → buildenv → Run workflow）；
3. 到 Packages 页面把 `a20os-buildenv` 设为 Public（fork 的 PR 才能免认证拉取）。

### ccache

CI 用 `actions/cache` 缓存 `build/cache/ccache`（按架构分 key，`restore-keys` 前缀回退）。本地容器构建想复用同一套缓存，把目录挂进去即可：`-v $PWD/build/cache/ccache:/src/build/cache/ccache`。

## ci.yml 的工作分解

```
buildenv-image（几秒）：把 owner 小写化，产出容器镜像名供其余 job 引用

toolchain-gates（裸 runner，不进容器，~20 s + 装 linux-headers）：
  全部是宿主侧、与架构无关、无需交叉工具链、无需 QEMU 的门禁，刻意不拉
  submodule（vendored 的 kernel/external/lwip 是 tracked tree，其余只读
  instances/、components/、tools/、Makefile、docs/ 和 kernel/ 的一方源文件）
    → make check-manifests
    → make check-a20-tests
    → make check-honesty-policy      # ~25 条 fail-closed 反回归断言
    → make check-smoke-cases
    → make host-tests
    → make check-drm-abi             # 需要宿主 linux-headers；跳过即判失败
    → make check-task-lifetime-boundary / check-smp-platform-boundary
      check-io-progress-model / check-external-dependency-boundary
    → make check-abi-boundary / check-envelope-coverage
    → make check-native-abi-coverage / check-abi-config-guard   # Native 侧同一件事

ci-kernel-arches（几秒）：make -s print-ci-kernel-arches，把 CI 的内核构建
  矩阵从 Makefile 的 CI_KERNEL_ARCHES 解析成 JSON 输出。矩阵列表不写在 YAML
  里——手抄的列表是第二真源，会在 SUPPORTED_HOSTED_ARCHES 之后悄悄落后。

kernel-build-<arch> ×7 并行（容器，无 submodule）：
    → make check-<arch>-bringup       # = make ARCH=<arch> BRINGUP=1 kernel-only
    → upload-artifact（kernel.elf；include-hidden-files 显式开启）
                                      # 架构专属构建断裂没有日志很难定位

build-<arch> ×4 并行：
  checkout（核心构建的第三方源码全部 vendored；actions/checkout 仍带 submodules: recursive 作为防御）
    → git safe.directory（容器内 root 跑 git 的常规处理）
    → 恢复 ccache
    → make dev-build            # 内核 + 用户态（沿用旧构建系统）
    → make pkg-repo             # 打 a20-base/a20-drivers/a20-kernel 三个包 + 签名建库
    → make image-world PKG_WORLD=base PKG_ALPINE=0   # 纯本地仓库组镜像
    → upload-artifact           # kernel.elf + 镜像 + 仓库包

smoke-riscv64（与 build 并行，riscv64，QEMU TCG）：
  checkout → git safe.directory → 恢复 ccache
    → make dev-build + pkg-repo # both-ABI dev 产物 + 包仓库（smoke-devtools 依赖）
    → QEMU 冒烟门禁（docs/testing-gates.md 核心 runtime 门禁）：
        smoke-riscv64           # bring-up + 主动关机（watchdog 超时即失败）
        smoke-abi-linux         # syscall_smoke
        smoke-syscall-ext       # keyring/AIO/io_uring/landlock 等扩展 syscall
        smoke-sched-stress / smoke-proc-stress / smoke-futex-stress
        smoke-sched-stress-smp2 / smoke-proc-stress-smp2  # 2 核 SMP 变体；单核 target 保留
        smoke-mm-stress / smoke-vfs-stress   # FAT/ext4/ISO9660 压力
        smoke-smp-bringup       # 2 核 SMP bring-up
        smoke-native-contract   # 唯一的 Native 运行时门禁（riscv64；见 docs/testing-gates.md
                                # 「Native 门禁的架构覆盖现状」：21 条 native smoke 全是 riscv64）
    → make smoke-devtools       # 上游 Alpine gcc 在 guest 内编译+运行
    → 失败也上传 .kernel-build/smoke/ 日志 artifact（smoke-riscv64-logs）
```

## CI 的内核构建矩阵

`SUPPORTED_HOSTED_ARCHES` 是六个活跃 hosted 架构的单一真源，CI 逐架构构建的也是这六项（`CI_KERNEL_ARCHES` = `riscv64 loongarch64 aarch64 x86_64 arm32 riscv32`，由`make print-ci-kernel-arches` 解析给 `strategy.matrix`）。ARM32 和 RISC-V32 仍在矩阵中； LoongArch32/PPC64LE 已按 1.0 平台决策退出，相关源码与历史门禁证据保留在文档和版本历史中，但不再安装旧工具链、不进入 active matrix。

`arm32` 曾因缺少架构 hook 导致编译异常，该缺陷修复后（两处故障路径改为经 `fault.c` 的架构 hook 调用）已纳入 CI 矩阵。若 arm32 在 CI 中报错，即为真实回归。

### 架构验证与交叉工具链边界

- **arm32 本地语法沙箱验证**：arm32 本地快速检查可使用宿主 gcc 语法沙箱——提取 `make ARCH=arm32 ABI=both BRINGUP=1 kernel-only -n` 编译命令，保留全部 `-I` / `-D` / `-Werror`，将交叉编译器替换为宿主 gcc、移除 ARM 专属代码生成标志，并加入 `-fsyntax-only`。该方式可快速验证头文件包含拓扑与声明可见性，但不能替代 ARM 真实指令生成与汇编验证，完整验证以 CI `kernel-build-arm32` 为准。
- **本地完整矩阵复验机制**：在配置交叉工具链环境下，可执行 `make -j8 check-build-matrix` 验证全架构 bring-up 内核及用户态。
- **构建目标分工**：`kernel-build` 是纯内核 bring-up 门禁，与 `check-kernel-build-all` 对应同一类构建目标；`build` 负责产出完整的发布包产物（apk 仓库与 world 镜像），需依赖用户态完整编译。
- **测试门禁调度策略**：`make check-doc-test-gates` 聚合了多个长耗时 QEMU 运行时测试。为了提高 CI 反馈速度并充分利用并发能力，CI 未直接调用单一聚合目标，而是将宿主侧子门禁与各独立 smoke job 拆分并行调度。

`smoke-devtools` 是唯一需要外网的 CI 步骤（从 Alpine 镜像站拉包；本地有`build/cache/apk` 缓存）。它是 trap.S 内核栈守卫修复的回归门禁。

注意：QEMU 在公共 runner 上没有 KVM，冒烟跑在 TCG 下；riscv64 QEMU 在 x86 宿主上本来就只有 TCG，各 smoke 目标的超时按 TCG 校准，适合 CI。

## release.yml 的工作分解

```
check-release-version（轻量 runner）：checkout → 校验 A20OS_VERSION 与
  A20OS_RELEASE 的 20.<version> 配对；tag ref 还要求 tag 严格等于 v<version>

每个架构并行：
  装发布密钥（默认要求 secret A20_REPO_SIGNING_KEY；缺失则失败）
  → dev-build → pkg-repo → image-world base + devel
  → 上传 artifact（镜像 + 各架构仓库目录 + 公钥）

release（等全部架构完成）：
  合并 artifact → 取注解 tag 正文作发布说明
  → softprops/action-gh-release 发 Release（附件 = 全部镜像）

该 job 刻意不挂 github-pages environment：该 environment 只允许 main
分支，带 environment 的 tag 触发会在任何 step 之前被拒绝，Release 就永远
建不出来。包仓库的对外发布（Pages）暂缓，见 repository.md。
```

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
| loongarch64 工具链缺失 | 容器默认装 `gcc-loongarch64-linux-gnu`（Debian cross-ports）；个别快照期缺失时按 docs/dev/build.md 用 Loongson 官方工具链 |
