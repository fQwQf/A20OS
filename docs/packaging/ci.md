# CI/CD 详解

最后核实：2026-08-27。

## 三个 workflow

| 文件 | 触发 | 职责 |
|------|------|------|
| `buildenv.yml` | `tools/ci/**` 变更 / 手动 | 构建构建容器镜像，推 `ghcr.io/<owner>/a20os-buildenv` |
| `ci.yml` | PR / push main / 手动 | 四架构矩阵构建 → 打包建库 → 组 base 镜像 → riscv64 QEMU 冒烟 → 上传 artifact |
| `release.yml` | tag `v*` / 手动 | 全架构发布构建 + base/devel 镜像 → 发布密钥签名 → GitHub Release + Pages 包仓库 |

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
checkout（不拉 submodule——核心构建的第三方源码全部 vendored）
  → git safe.directory（容器内 root 跑 git 的常规处理）
  → 恢复 ccache
  → make dev-build            # 内核 + 用户态（沿用旧构建系统）
  → make pkg-repo             # 打 a20-base/a20-drivers/a20-kernel 三个包 + 签名建库
  → make image-world PKG_WORLD=base PKG_ALPINE=0   # 纯本地仓库组镜像
  → (仅 riscv64) make smoke-riscv64                # QEMU TCG 冒烟
  → (仅 riscv64) make smoke-devtools               # 上游 Alpine gcc 在 guest 内编译+运行
  → upload-artifact           # kernel.elf + 镜像 + 仓库包
```

`smoke-devtools` 是唯一需要外网的 CI 步骤（从 Alpine 镜像站拉包；本地有
`build/cache/apk` 缓存）。它是 trap.S 内核栈守卫修复的回归门禁。

注意：QEMU 在公共 runner 上没有 KVM，冒烟跑在 TCG 下，`smoke-riscv64`
的 watchdog 语义本来就是"超时即通过"（见根 README），因此适合 CI。

## release.yml 的工作分解

```
每个架构并行：
  装发布密钥（secret A20_REPO_SIGNING_KEY；缺失则不签名 + 醒目警告）
  → dev-build → pkg-repo → image-world base + devel
  → 上传 artifact（镜像 + 各架构仓库目录 + 公钥）

publish（等全部架构完成）：
  合并 artifact → 组装站点目录（repo/<arch>/* + 公钥 + index.md）
  → softprops/action-gh-release 发 Release（附件 = 全部镜像）
  → actions/deploy-pages 部署包仓库到 GitHub Pages
```

### 发布准备（一次性）

1. **Pages**：Settings → Pages → Build and deployment → Source 选
   "GitHub Actions"；
2. **发布密钥**（可选但推荐）：
   ```bash
   openssl genrsa -out a20os-release.rsa 4096
   openssl rsa -in a20os-release.rsa -pubout -out a20os-release.rsa.pub
   ```
   私钥内容粘进 Settings → Secrets → Actions 的
   `A20_REPO_SIGNING_KEY`；公钥会随 Pages 站点自动发布，无需入库；
3. 打 tag：`git tag v0.3.0 && git push --tags`。

## 常见故障

| 现象 | 原因与处理 |
|------|-----------|
| `UNTRUSTED signature` | 消费时缺 `--keys-dir` 或公钥名与打包时 `--key-name` 不一致 |
| `no such package` 且仓库明明有 | 索引签名不被信任会**静默丢弃整个仓库**——检查 keys-dir 是否传了**绝对路径**（apk 对相对 keys-dir 会因内部 chdir 而失效，mkrootfs 已代为绝对化，手工调用 apk 时注意） |
| `unexpected end of file`（读包时） | 包不是 mka20pkg 产物：apk v2 的分段 tar 格式约束见 [apk-format.md](apk-format.md) |
| 容器里 `git` 报 dubious ownership | 加 `git config --global --add safe.directory "$GITHUB_WORKSPACE"`（workflow 已含） |
| loongarch64 工具链缺失 | 容器默认装 `gcc-loongarch64-linux-gnu`（Debian cross-ports）；个别快照期缺失时按 docs/build.md 用 Loongson 官方工具链 |
