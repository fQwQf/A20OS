# A20OS 包管理与镜像分发

A20OS 使用 apk（Alpine 包格式）作为包管理与镜像组装体系：
自有组件（内核/用户态/驱动/移植软件）由 recipe 打成 apk 包，
与 Alpine 上游包在同一个解析器下按 world 清单组合成镜像，
经 GitHub Actions 构建、签名、发布。

## 阅读顺序

1. [overview.md](overview.md) — 为什么、五层架构、核心概念
2. [quickstart.md](quickstart.md) — 一分钟跑通与常用命令
3. [packages.md](packages.md) — recipe 格式参考与新增包流程
4. [images.md](images.md) — world 清单与镜像组装
5. [repository.md](repository.md) — 仓库、签名与密钥、Pages 托管
6. [ci.md](ci.md) — 三条 workflow、本地容器复现、排错
7. [apk-format.md](apk-format.md) — apk v2 格式深挖（改打包器前必读）
8. [migration.md](migration.md) — 新旧对照与迁移路线

## 工具速查

| 工具 | 作用 |
|------|------|
| `tools/mka20pkg.py` | recipe → apk 包（可选 RSA256 签名） |
| `tools/mka20repo.sh` | 目录中的 *.apk → 带索引（可选签名）的仓库 |
| `tools/apk-sign-stream.py` | 给索引等 gzip 流前置签名段 |
| `tools/ensure-apk-static.sh` | 获取/缓存静态 apk 二进制 |
| `tools/mkrootfs.py` | world 清单 → ext4 rootfs 镜像 |

Make 入口：`make pkgs` / `make pkg-repo` / `make image-world` /
`make pkgs-check` / `make pkg-key`（变量见 `tools/targets-pkg.mk` 头部注释）。
