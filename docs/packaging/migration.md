# 新旧体系对照与迁移路线

最后核实：2026-10-01。

## 原则：叠加，不替换

新包体系是**叠加层**。以下旧入口全部原样保留、行为不变：

- `make run` / `make dev-build` / 全部 `smoke-*` 门禁；
- `user/rootfs/alpine/build.sh`（`make rootfs-alpine`、`make distro-run`）；
- 内核 ramfs 内嵌早期驱动（EARLY_DRVMOD，启动根盘所需，永远在内核里）。

新体系的打包器**只读构建产物目录**，不影响构建过程本身。

**唯一的例外是源码构建的 extra 路径，它已退役**。`user/extra.mk` 已删除，
`user/external/` 下的 vim、git、zlib、curl+mbedtls、binutils+GCC、Rust、
Lamina1 源码树也不再入库：这些软件现在由 world 清单从 Alpine 上游仓库解析。
`extra-img`、`run-*-extra`、`vf2-extra` 这些入口刻意保留为**会报错的弃置桩**
（`tools/targets-extra.mk`），每个都打印替代命令后 `exit 2`，而不是让 make
报一句什么也不教的 "no rule to make target"。

`/extra` 这个**分区本身没有取消**：真板仍需要一块可写 ext4，`make vf2-sdcard`
现在用 apk world 镜像填它（`VF2_WORLD` 选 world，默认 `devel`）。

## 对照表

| 旧做法 | 新做法 | 说明 |
|--------|--------|------|
| 用户程序 `objcopy` 成 `.o` 链进内核（RAMFS_USER blob） | `a20-base` 包 → world 组装进 rootfs | 解耦内核与用户态构建 |
| `mcopy` 逐个塞 FAT32 / `mkfs -d` 暂存目录 | `mkrootfs.py` + world 清单 | 内容有清单、有版本、有依赖 |
| 已删除的 `user/extra.mk` 里 `vim) stamp=.vim-built ;;` 硬编码 case | world 清单里直接写 Alpine 上游包名 | 加包 = 往 `packages/world/*.world` 加一行 |
| extra 镜像（独立 ext4 挂 /extra） | extra 包进仓库，world 引用 | 保留 /extra 布局，见下 |
| 发布 = 手工拷贝 `disk.img` | tag → release workflow → Release + Pages 仓库 | 自动、多架构、带签名 |
| 环境 = 宿主机手装 apt 依赖 | `tools/ci/Dockerfile` 固化 | CI 与本地同一容器 |

## 迁移路线（建议顺序，每一步都可独立落地）

1. **CI 先行**：启用 buildenv → ci.yml。不改任何开发者习惯，立刻获得
   四架构 PR 门禁与 artifact。
2. **打包并行运行**：日常使用 `make pkgs / pkg-repo / image-world`，
   与旧 `disk.img` 并存验证一段时间（两者内容应等价，a20-base 就是
   按旧镜像内容对齐的）。
3. **源码构建的 userland 退役（已完成）**：`user/extra.mk` 与
   `user/external/` 下的 vendored 树（vim、git、zlib、curl+mbedtls、
   binutils+GCC、Rust、Lamina1）全部删除，移植软件一律作为 Alpine
   上游包直接写进 `packages/world/*.world`：`devel` 带 musl busybox
   vim git curl ca-certificates less，`devtools` 再加 gcc musl-dev
   fastfetch。`a20-extra-git`/`a20-extra-vim` 这两个孤儿 recipe 一并
   删除（它们不在任何 world 里，也不在 `PKG_RECIPES` 中）。仍然从
   源码构建的用户程序只剩 `user/cmds/` 与 fastfetch。
4. **distro 路径归并**：`user/rootfs/alpine/packages.txt` 实际就是一份
   world。后续可将 `desktop.world` 纳入 packages/world/，让
   `build.sh` 变成 mkrootfs 的薄封装（保留 chroot init 等 overlay
   逻辑）；在此之前两者并存。
5. **淘汰 objcopy-进内核（未做）**：`user/extra.mk` 已退役，但
   RAMFS_USER blob 这条路径仍在，尚未移除。等 base.world 镜像在所有
   日常流程中稳定替代 disk.img 之后再动它（注意保留 EARLY_DRVMOD，
   那是根盘驱动的引导路径，不是同一回事）。

## 兼容性承诺

- 旧 make 目标不删不改，直到对应能力在新体系有等价物且经过验证；
- 包内布局对齐旧镜像（`a20-base` 平铺根目录、`/lib/drivers`、
  `/musl/lib/libc.so`、extra 包 `/extra/...`），init 与脚本无需改动；
- 若发现新旧产物不一致，以旧路径行为为准并提 issue，对齐是
  新体系的责任。
