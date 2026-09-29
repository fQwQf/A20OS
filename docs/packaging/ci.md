# CI/CD 详解

最后核实：2026-09-14。

## 三个 workflow

| 文件 | 触发 | 职责 |
|------|------|------|
| `buildenv.yml` | `tools/ci/**` 变更 / 手动 | 构建构建容器镜像，推 `ghcr.io/<owner小写>/a20os-buildenv` |
| `ci.yml` | PR / push main / 手动 | 宿主侧源码契约门禁（快速）→ 逐架构内核 bring-up 构建矩阵 → 四架构打包建库 + 组 base 镜像 → 独立 smoke job 跑 riscv64 全套 QEMU 冒烟 → 上传 artifact |
| `release.yml` | tag `v*` / 手动 | 全架构发布构建 + base/devel 镜像 → 发布密钥签名 → GitHub Release（附件 = 各架构镜像） |

镜像名小写：ghcr 镜像名必须全小写，而 `github.repository_owner` 可能含大写
（如 `fQwQf`，buildx 会直接报 `repository name must be lowercase`）。三个
workflow 都先经一个 `buildenv-image` 解析 job 用 shell 小写化 owner，再在
`container.image` / build-push `tags` 里引用结果；`container.image` 在 job
启动前解析、用不了步骤内 shell，所以必须走独立的 resolver job。

## 构建容器（环境可复现的关键）

`tools/ci/Dockerfile` 基于 `debian:trixie-slim`，固化：

- 全架构交叉工具链（riscv64 / aarch64 / x86_64 / loongarch64 /
  ppc64le / arm，含 `riscv64-unknown-elf` 裸机工具链）；
- QEMU（riscv64/x86/arm）、镜像工具（mtools / dosfstools / e2fsprogs）、
  `fakeroot`、`ccache`、`openssl`、Python 3。

依赖清单刻意与 [../build.md](../build.md) 的"环境准备"一节一一对应；
**改动一边时必须同步另一边**。

CI 的所有 job 通过 `container:` 运行在这个镜像里，因此"CI 挂了本地
复现"就是一条命令：

```bash
docker run --rm -it -v "$PWD:/src" -w /src \
    ghcr.io/<owner>/a20os-buildenv:latest bash
# 容器内：make ARCH=riscv64 dev-build
```

### 首次启用 / 更新镜像

`buildenv.yml` 需要推 ghcr，首次使用前：

1. 仓库 Settings → Actions → General → Workflow permissions 设为
   "Read and write permissions"（GITHUB_TOKEN 才能推 packages）；
2. 手动触发一次 buildenv（Actions → buildenv → Run workflow）；
3. 到 Packages 页面把 `a20os-buildenv` 设为 Public（fork 的 PR 才能
   免认证拉取）。

### ccache

CI 用 `actions/cache` 缓存 `build/cache/ccache`（按架构分 key，
`restore-keys` 前缀回退）。本地容器构建想复用同一套缓存，把目录
挂进去即可：`-v $PWD/build/cache/ccache:/src/build/cache/ccache`。

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

ci-kernel-arches（几秒）：make -s print-ci-kernel-arches，把 CI 的内核构建
  矩阵从 Makefile 的 CI_KERNEL_ARCHES 解析成 JSON 输出。矩阵列表不写在 YAML
  里——手抄的列表是第二真源，会在 SUPPORTED_HOSTED_ARCHES 之后悄悄落后。

kernel-build-<arch> ×6 并行（容器，无 submodule）：
    → make check-<arch>-bringup       # = make ARCH=<arch> BRINGUP=1 kernel-only
    → upload-artifact（kernel.elf）    # 架构专属构建断裂没有日志很难定位

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
        smoke-mm-stress / smoke-vfs-stress   # FAT/ext4/ISO9660 压力
        smoke-smp-bringup       # 2 核 SMP bring-up
    → make smoke-devtools       # 上游 Alpine gcc 在 guest 内编译+运行
    → 失败也上传 .kernel-build/smoke/ 日志 artifact（smoke-riscv64-logs）
```

## CI 的内核构建矩阵

`SUPPORTED_HOSTED_ARCHES` 是七架构的单一真源，但 CI 只构建其中六个
（`CI_KERNEL_ARCHES` = `riscv64 loongarch64 aarch64 x86_64 riscv32 ppc64le`，
由 `make print-ci-kernel-arches` 解析给 `strategy.matrix`）。两个架构被显式
排除，理由写在 `Makefile` 的 `CI_KERNEL_ARCHES` 注释里，不是静默丢弃：

| 架构 | 排除原因 |
|------|----------|
| `arm32` | 工具链在镜像里（`gcc-arm-linux-gnueabihf`），但 `kernel/mm/fault.c` 使用了 `mm_addrspace_lock()` / `mm_cursor_*` 事务接口，而 `kernel/include/mm/pt.h` 只在 `ARCH_HAS_PGTABLE_OPS` 下声明它们，`Makefile` 又刻意不给 arm32 这个宏（它有自己的短描述符页表后端 `kernel/arch/arm32/mm/pgtbl.c`）。`check-arm32-bringup` 当前编译不过。修复属于内核代码，所以在修好之前把 arm32 放进 CI 只会让 CI 一直红着。 |
| `loongarch32` | LA32R 没有发行版交叉工具链可装：Debian 不提供 loongarch32 gcc，项目是从源码构建 cloudspurs 的 binutils/gcc la32 分支（[platforms/loongarch32.md](../platforms/loongarch32.md)）。`tools/ci/Dockerfile` 无法 apt-get 一个上游不存在的包。它同时也不是 hosted 架构，因此不在 `SUPPORTED_HOSTED_ARCHES` 里，`check-kernel-build-all` 中单独的 `check-loongarch32-bringup` 仍只在本机跑。 |

`kernel-build` 与 `build` 分开不是重复：`build` 要产出发布打包产物
（apk 仓库 + world 镜像），因此需要可用的用户态，而 riscv32 的用户态当前
构建不过（`user/cmds/stress/poll_edge.c` 在 riscv32 上引用了 musl 未声明的
`SYS_pselect6` / `SYS_ppoll`）；`kernel-build` 是纯内核 bring-up 门禁，与
`check-kernel-build-all` 迭代的是同一个 `check-<arch>-bringup` 目标。

`make check-doc-test-gates` 目前**不在** CI 里，原因有两条，都不是"忘了"：
它不是快速的纯文档检查——17 个子门禁里有 11 个各自拉起 QEMU guest，按
[testing-gates.md](../testing-gates.md) 的说明属于长时间聚合；而且它当前
在树上是红的（`check-task-state-boundary`、`check-abi-smoke-gate`、
`check-doc-drift` 三项失败）。`check-final-definition` 的 11 条断言已被
`check-doc-test-gates` 完全覆盖，所以 CI 只跑后者，不重复跑同一批 QEMU
smoke。

`smoke-devtools` 是唯一需要外网的 CI 步骤（从 Alpine 镜像站拉包；本地有
`build/cache/apk` 缓存）。它是 trap.S 内核栈守卫修复的回归门禁。

注意：QEMU 在公共 runner 上没有 KVM，冒烟跑在 TCG 下；riscv64 QEMU 在
x86 宿主上本来就只有 TCG，各 smoke 目标的超时按 TCG 校准，适合 CI。

## release.yml 的工作分解

```
每个架构并行：
  装发布密钥（secret A20_REPO_SIGNING_KEY；缺失则不签名 + 醒目警告）
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

1. **发布密钥**（可选但推荐）：
   ```bash
   openssl genrsa -out a20os-release.rsa 4096
   openssl rsa -in a20os-release.rsa -pubout -out a20os-release.rsa.pub
   ```
   私钥内容粘进 Settings → Secrets → Actions 的
   `A20_REPO_SIGNING_KEY`；公钥由 workflow 从私钥导出并随 artifact 上传，
   无需入库；
2. 打注解 tag（tag 正文会成为 Release 的发布说明）：
   `git tag -a v0.13 -m "..." && git push origin v0.13`。

Pages（Settings → Pages → Source 选 "GitHub Actions"）只服务于 `pages.yml`
的官网站点，与 release 无关。

## 常见故障

| 现象 | 原因与处理 |
|------|-----------|
| `UNTRUSTED signature` | 消费时缺 `--keys-dir` 或公钥名与打包时 `--key-name` 不一致 |
| `no such package` 且仓库明明有 | 索引签名不被信任会**静默丢弃整个仓库**。检查 keys-dir 是否传了**绝对路径**（apk 对相对 keys-dir 会因内部 chdir 而失效，mkrootfs 已代为绝对化，手工调用 apk 时注意） |
| `unexpected end of file`（读包时） | 包不是 mka20pkg 产物：apk v2 的分段 tar 格式约束见 [apk-format.md](apk-format.md) |
| 容器里 `git` 报 dubious ownership | 加 `git config --global --add safe.directory "$GITHUB_WORKSPACE"`（workflow 已含） |
| loongarch64 工具链缺失 | 容器默认装 `gcc-loongarch64-linux-gnu`（Debian cross-ports）；个别快照期缺失时按 docs/build.md 用 Loongson 官方工具链 |
