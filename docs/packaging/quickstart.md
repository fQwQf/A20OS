# 快速上手（Quickstart）

最后核实：2026-08-27。

所有命令在项目根目录执行。首次使用只需记得三个入口：
`make pkgs`（打包）→ `make pkg-repo`（建仓库）→ `make image-world`（组镜像），
而 `image-world` 会自动带上前两者，所以最常用的就是最后一条。

## 一分钟跑通

```bash
# 1. 构建内核 + 用户态（和往常一样）
make ARCH=riscv64 dev-build

# 2. 打包 + 建本地仓库 + 按 base 清单组镜像
make ARCH=riscv64 image-world PKG_WORLD=base PKG_ALPINE=0

# 产物：
#   build/packages/riscv64/*.apk        三个包：a20-base / a20-drivers / a20-kernel
#   build/repo/riscv64/APKINDEX.tar.gz  本地仓库索引（已用 dev 密钥签名）
#   build/images/base-riscv64.img       ext4 rootfs 镜像
```

首次运行会自动生成本地开发签名密钥 `build/keys/a20os-dev.rsa`
（在 `.gitignore` 覆盖的 `build/` 下，不会被提交）。

## 常用命令

| 目的 | 命令 |
|------|------|
| 只打包（不建库） | `make ARCH=riscv64 pkgs` |
| 只打包指定包 | `make pkgs PKG_RECIPES="a20-base a20-drivers"` |
| 校验 recipe 不写包 | `make pkgs-check` |
| 组 devel 镜像（含 Alpine 上游 vim/git） | `make ARCH=riscv64 image-world PKG_WORLD=devel PKG_SIZE_MB=1024` |
| 组镜像但不碰网络 | `make image-world PKG_ALPINE=0` |
| 手工组装（更细控制） | `tools/mkrootfs.py --arch riscv64 --world packages/world/base.world --repo build/repo --keys-dir build/keys --no-alpine` |
| 检查包内容 | `tar -tzf build/packages/riscv64/a20-base-*.apk`（或用 `apk.static` 解包） |

## 打包 extra 软件（vim/git 等从源码移植的）

extra 包的构建仍走旧流程（`user/extra.mk`），新体系负责把产物变成包：

```bash
make ARCH=riscv64 extra-user-apps EXTRA_PACKAGES=vim     # 旧流程构建
make ARCH=riscv64 pkgs PKG_RECIPES=a20-extra-vim         # 新流程打包
```

## 在容器里构建（与 CI 完全相同的环境）

```bash
docker build -t a20os-buildenv tools/ci
docker run --rm -v "$PWD:/src" -w /src a20os-buildenv \
    make ARCH=riscv64 dev-build
```

宿主机非 Debian 13 时强烈建议用容器；CI 用的正是这个镜像。

## 发布一个版本

```bash
git tag v0.3.0 && git push --tags
# → release workflow 自动构建四架构镜像、签名包仓库、
#   创建 GitHub Release、把仓库部署到 GitHub Pages
```

一次性设置见 [ci.md](ci.md) 的"发布准备"一节。

## 下一步

- 想给自己的程序做包 → [packages.md](packages.md)
- 想定义新的镜像组合 → [images.md](images.md)
- 想搞清签名/信任 → [repository.md](repository.md)
