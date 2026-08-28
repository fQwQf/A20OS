# 镜像组装：world 清单与 mkrootfs

最后核实：2026-08-27（与 `tools/mkrootfs.py` 同步维护）。

## world 文件：镜像即清单

一个 world 文件定义一张镜像的内容——每行一个 apk 包名：

```
# packages/world/devel.world
a20-base
a20-drivers

# 以下是 Alpine 上游包（联网组装时从镜像站拉取）
musl
busybox
vim
git
```

语法规则：

- `#` 开头（或行内 `#` 之后）是注释；空行忽略；
- 支持 apk 的版本约束语法：`vim=9.1.*`、`git>=2.4` 等；
- 一行一个包，不允许行内空格。

**组合镜像 = 编辑清单**。这正是旧体系做不到的事：要一个"带 vim 的最小
系统"，复制 base.world、加一行 `vim`，组装即可，不需要改任何 Makefile。

内置 profile：

| world | 内容 | 网络需求 |
|-------|------|----------|
| `base` | a20-base + a20-drivers（纯自有，对应旧 disk.img 的内容） | 无 |
| `devel` | base + Alpine 上游 musl/busybox/vim/git/curl 等 | 需要访问 Alpine 镜像站 |

> 完整的 XFCE 桌面发行版路径（`make distro-run`，chroot 进 Alpine
> rootfs）仍然由 `user/rootfs/alpine/` 承载；它本身就是同一思路的先驱，
> 后续可作为 `desktop.world` 迁入本体系，见 [migration.md](migration.md)。

## 组装：`mkrootfs.py`

```bash
tools/mkrootfs.py --arch riscv64 \
    --world packages/world/devel.world \
    --repo build/repo \              # 本地 A20OS 仓库（默认即此）
    --output build/images/devel-riscv64.img \
    --size-mb 1024
```

工作流程：

1. 确保有静态 `apk`（自动下载 `apk-tools-static`，缓存于
   `build/cache/apk-tools/`）；
2. 用真正的 apk 解析器把 world 清单 + 依赖解析到 staging 目录
   （`apk --root <staging> --arch <arch> --initdb add ...`）；
3. 应用 `--overlay` 目录（可选，层层覆盖）；
4. 写入 `/etc/apk/repositories`（仅远程仓库），让镜像内的系统
   将来也能 `apk add`；
5. `mkfs.ext4 -d staging` 产出镜像。

### 权限与 --usermode

正确属主（root 拥有文件）需要 root 权限。脚本按此顺序处理：

- 已是 root（CI 容器）→ 直接做；
- 非 root → 默认对 apk/mkfs 步骤加 `sudo`（与旧 alpine 脚本一致）；
- `--usermode` → 完全不要 sudo：文件以调用者身份落盘，再用
  `fakeroot` + `root_owner=0:0` 让**镜像内**文件属主仍为 root。
  开发期最省心。`make image-world` 检测到非 root 会自动加此开关。

### 仓库来源与信任

- `--repo` 可多次给出；本地目录会自动拼接 `/<arch>/`（注意：传给 apk 的
  是仓库根，apk 自己追加架构目录）；URL 原样使用；
- 默认追加 Alpine `main` + `community`（`--alpine-mirror` 换镜像站，
  默认 USTC；`--no-alpine` 完全关闭）；
- 签名验证：`--keys-dir` 指向公钥目录（本地 dev 公钥在 `build/keys/`）；
  Alpine 官方公钥由脚本自动引导获取（缓存于 `build/cache/alpine-keys/`）；
- 完全离线/不在意验证时可用 `--allow-untrusted`（日志会有告警）。

### 输出格式

- `--format ext4`（默认）：raw ext4 镜像，可直接作为 QEMU 的
  `-drive ... format=raw` 或根盘。特性集与旧构建一致
  （`^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index`，
  与内核 ext4 实现验证过的集合相同）；
- `--format dir`：不打包镜像，把 staging 目录搬到 `--output`，
  适合调试内容或做进一步加工。

### 缓存

- apk 包缓存：`build/cache/apk/<arch>/`（重复组装不同 world 时上游包
  不会重复下载）；
- 清掉缓存即可强制全新拉取。

## 通过 make 使用

```bash
make image-world ARCH=riscv64 PKG_WORLD=base   # 打包+建库+组镜像
make run-world   ARCH=riscv64 PKG_WORLD=base   # 组镜像并直接在 QEMU 启动
```

`run-world` 把 world 镜像挂为第二块盘（内核枚举为 ext4 → 挂载到 `/extra`；
若镜像内含 `/etc/a20-distro` 标记与 `/sbin/init`，则由 init 自动 chroot
接管进入 distro 模式——guest 直接落在 world rootfs 里，无需手动 chroot），
根盘仍是常规 FAT32 开发镜像（→ `/bin`）。

组镜像时若存在 `packages/overlay/<world 名>/` 目录，`image-world` 会自动
将其作为 overlay 应用（覆盖同名文件，例如用自定义 `/sbin/init` 顶替
busybox 的符号链接）。devtools world 即以此方式提供 distro 直通：
`packages/overlay/devtools/` 内含 `etc/a20-distro` 标记与 stage-2 init
脚本，`make run-world PKG_WORLD=devtools` 启动后直接进入 Alpine shell。

| 变量 | 默认 | 含义 |
|------|------|------|
| `PKG_WORLD` | `base` | world 清单名（packages/world/\<name\>.world） |
| `PKG_SIZE_MB` | `512` | 镜像大小 |
| `PKG_ALPINE` | `1` | 是否引入 Alpine 仓库；纯本地组合设 `0` |
| `PKG_REPO_DIR` | `build/repo` | 本地仓库位置 |

## 验证镜像内容（不起 QEMU）

```bash
debugfs -R "ls /lib/drivers" build/images/base-riscv64.img
debugfs -R "cat /etc/os-release" build/images/base-riscv64.img
```

## 启动组装出的镜像

```bash
make run-world ARCH=riscv64 PKG_WORLD=base
```

它复用根 Makefile 的 QEMU 配置（不要手抄 QEMU 参数——各平台的
`-bios default`、virtio 总线槽位等易错细节都在 `Makefile`/`tools/run-targets.mk`
里维护），把 world 镜像作为第二块盘启动。镜像是不含分区表的 raw ext4，
也可以手工挂到任何现有 `run-*`/`distro-run` 命令的第二盘位。
