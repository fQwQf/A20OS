# 新旧体系对照与迁移路线

最后核实：2026-08-27。

## 原则：叠加，不替换

新包体系是**叠加层**。以下旧入口全部原样保留、行为不变：

- `make run` / `make dev-build` / 全部 `smoke-*` 门禁；
- `user/rootfs/alpine/build.sh`（`make rootfs-alpine`、`make distro-run`）；
- `user/extra.mk` 与 `make extra-img` / `run-*-extra`；
- 内核 ramfs 内嵌早期驱动（EARLY_DRVMOD，启动根盘所需，永远在内核里）。

新体系的打包器**只读构建产物目录**，不影响构建过程本身。

## 对照表

| 旧做法 | 新做法 | 说明 |
|--------|--------|------|
| 用户程序 `objcopy` 成 `.o` 链进内核（RAMFS_USER blob） | `a20-base` 包 → world 组装进 rootfs | 解耦内核与用户态构建 |
| `mcopy` 逐个塞 FAT32 / `mkfs -d` 暂存目录 | `mkrootfs.py` + world 清单 | 内容有清单、有版本、有依赖 |
| `user/extra.mk` 里 `vim) stamp=.vim-built ;;` 硬编码 case | `packages/recipes/a20-extra-vim.toml` | 加包 = 加一个 recipe 文件 |
| extra 镜像（独立 ext4 挂 /extra） | extra 包进仓库，world 引用 | 保留 /extra 布局，见下 |
| 发布 = 手工拷贝 `disk.img` | tag → release workflow → Release + Pages 仓库 | 自动、多架构、带签名 |
| 环境 = 宿主机手装 apt 依赖 | `tools/ci/Dockerfile` 固化 | CI 与本地同一容器 |

## 迁移路线（建议顺序，每一步都可独立落地）

1. **CI 先行**：启用 buildenv → ci.yml。不改任何开发者习惯，立刻获得
   四架构 PR 门禁与 artifact。
2. **打包并行运行**：日常使用 `make pkgs / pkg-repo / image-world`，
   与旧 `disk.img` 并存验证一段时间（两者内容应等价——a20-base 就是
   按旧镜像内容对齐的）。
3. **extra 包 recipe 化**：新的移植软件直接写 recipe，不再往
   `extra.mk` 的 case 列表里加；存量 vim/git 已有 recipe，gcc/rust/
   lamina 的 recipe 化作为后续工作（它们的构建逻辑复杂，先保持
   extra.mk 构建、逐步把"安装布局"搬进 recipe）。
4. **distro 路径归并**：`user/rootfs/alpine/packages.txt` 实际就是一份
   world。后续可将 `desktop.world` 纳入 packages/world/，让
   `build.sh` 变成 mkrootfs 的薄封装（保留 chroot init 等 overlay
   逻辑）；在此之前两者并存。
5. **淘汰 objcopy-进内核**：当 base.world 镜像在所有日常流程中稳定
   替代 disk.img 后，再考虑移除 RAMFS_USER blob 路径（注意保留
   EARLY_DRVMOD——那是根盘驱动的引导路径，不是同一回事）。

## 兼容性承诺

- 旧 make 目标不删不改，直到对应能力在新体系有等价物且经过验证；
- 包内布局对齐旧镜像（`a20-base` 平铺根目录、`/lib/drivers`、
  `/musl/lib/libc.so`、extra 包 `/extra/...`），init 与脚本无需改动；
- 若发现新旧产物不一致，以旧路径行为为准并提 issue——对齐是
  新体系的责任。
