# A20OS 包管理与镜像分发体系总览

最后核实：2026-08-27（对应提交引入 `tools/mka20pkg.py` 等工具时）。

## 为什么要有这套体系

旧流程里，所有用户态程序在编译期被 `objcopy` 成目标文件**链接进内核镜像**，
或者被 `mcopy` 逐个塞进 FAT32 镜像。这带来三个结构性问题：

1. **不可组合**——想要"基础系统 + vim"还是"基础系统 + 完整桌面"，只能改
   Makefile、重新全量构建；
2. **不可分发**——产物是整盘镜像，没有"包"这个粒度，第三方无法只取一个
   程序或只更新一个驱动；
3. **不可复现**——构建依赖宿主机手工安装的交叉工具链，换台机器结果就可能
   不同，也没有 CI 兜底。

## 核心决策：复用 apk，而不是自研包管理器

A20OS 拥有完整的 Linux ABI 兼容层，能直接运行 musl/Alpine 生态的程序。
因此用户态包管理**不需要重新发明**：Alpine 的 apk 格式简单（gzip 分段
tar + 文本元数据）、工具体积小（单个静态二进制）、支持异架构离线组装
（`apk --root --arch`），且上游仓库 already 提供 riscv64 / aarch64 /
x86_64 / loongarch64 的大量现成包。

项目里已有的 `user/rootfs/alpine/build.sh` 正是用 `apk.static` 组装
Alpine rootfs——新体系把这一已被验证的做法**泛化到整个系统**：A20OS 自己的
内核、用户态、驱动也打成 apk 包，与上游包在同一个解析器下混装。

## 五层架构

```
┌─────────────────────────────────────────────────────────────┐
│ 5. 发布层   .github/workflows/release.yml                     │
│             tag → 多架构镜像 → GitHub Release + Pages 包仓库   │
├─────────────────────────────────────────────────────────────┤
│ 4. CI 层    .github/workflows/ci.yml（PR 门禁）               │
│             tools/ci/Dockerfile（固化工具链，CI=本地）          │
├─────────────────────────────────────────────────────────────┤
│ 3. 组装层   packages/world/*.world  +  tools/mkrootfs.py      │
│             声明式包清单 → apk 解析 → ext4 镜像                │
├─────────────────────────────────────────────────────────────┤
│ 2. 仓库层   tools/mka20repo.sh + tools/apk-sign-stream.py     │
│             *.apk → APKINDEX.tar.gz（可签名）→ build/repo/     │
├─────────────────────────────────────────────────────────────┤
│ 1. 打包层   packages/recipes/*.toml  +  tools/mka20pkg.py     │
│             构建产物 → 合法 apk v2 包（可 RSA256 签名）          │
└─────────────────────────────────────────────────────────────┘
                ↑ 数据全部来自现有构建系统（make 不变）
```

**构建系统本身不变**：内核与用户态仍由根 `Makefile` 构建。新体系只消费
构建产物（`user/build/<variant>/`、`.kernel-build/.../kernel.elf` 等），
把它们变成可组合、可签名、可分发的包。

## 四个核心概念

| 概念 | 载体 | 类比 |
|------|------|------|
| **recipe** | `packages/recipes/<name>.toml` | APKBUILD / PKGBUILD：描述"哪些构建产物 + 元数据 = 哪个包" |
| **包 (.apk)** | `build/packages/<arch>/*.apk` | 标准 Alpine apk v2 包，上游 apk 工具可直接操作 |
| **仓库 (repo)** | `build/repo/<arch>/` + `APKINDEX.tar.gz` | apk 仓库；发布时就是 GitHub Pages 上的静态目录 |
| **world** | `packages/world/<profile>.world` | 一个"镜像配方"：要装哪些包的清单（Alpine 同名概念） |

一条命令走完全程：

```bash
make ARCH=riscv64 image-world PKG_WORLD=base
# = make pkgs（打包）→ make pkg-repo（建库）→ mkrootfs（按 world 组装镜像）
```

## 包从哪来：两类来源

1. **A20OS 自有包**（`a20-*`）：由 recipe 从本仓库构建产物生成，
   进本地/发布仓库。内核、基础用户态、驱动、extra 移植软件属此类。
2. **Alpine 上游包**：`vim`、`git`、`busybox`、`xfce4`……直接在 world
   里写包名，组装时从 Alpine 镜像站拉取并验证官方签名。上游包覆盖不到的
   架构（如 loongarch64 在上游是社区移植）可只用自己的仓库
   （`PKG_ALPINE=0`）。

## 信任链

- 包和仓库索引都可以 RSA/SHA-256 签名（与 abuild 同一方案，见
  [apk-format.md](apk-format.md)）；
- 本地开发用自动生成的 dev 密钥（`build/keys/`，不入库）；
- 正式发布用 CI secret 注入的发布密钥（见 [repository.md](repository.md)）；
- Alpine 上游包用 Alpine 官方公钥验证（mkrootfs 自动引导获取）。

## 与旧路径的关系

**旧路径完全保留、继续可用**：`make run`、`make distro-run`、
`user/rootfs/alpine/build.sh`、`user/extra.mk` 均未改动。新体系是叠加层，
详见 [migration.md](migration.md) 的对照表与迁移路线。

## 文档地图

| 文档 | 内容 |
|------|------|
| [quickstart.md](quickstart.md) | 常用命令速查（打包/建库/组镜像/发布） |
| [packages.md](packages.md) | recipe 格式参考、如何新增一个包 |
| [images.md](images.md) | world 清单与镜像组装详解 |
| [repository.md](repository.md) | 仓库布局、签名与密钥管理、Pages 托管 |
| [ci.md](ci.md) | GitHub Actions 流水线、本地容器复现、排错 |
| [apk-format.md](apk-format.md) | apk v2 包格式深挖（我们的包为什么长这样） |
| [migration.md](migration.md) | 新旧体系对照与迁移路线 |
