# THIRD-PARTY NOTICES

本文件索引 A20OS 源码与构建产物可能包含的第三方组件。它不是法律意见，也不能替代针对实际分发物、精确源码 revision 和对应 LICENSE/COPYING 的核验。

## 1. 分发边界声明

A20OS 仓库不能预先限定下游只分发源码。`fat32.img`、`ext4.img`、world 镜像（`build/images/*.img`）、ISO、独立可执行文件和共享库一旦提供给第三方，就是需要按其实际内容审查的二进制/资源分发物。因此：

- `kernel/external/lwip` 和 `user/external/{musl,mlibc,mksh-cvs2git,sbase,tlse}` 是普通 tracked tree；不能称为 submodule。
- `.gitmodules` 目前只登记 `user/external/apps/fastfetch` 一项；超级项目的 gitlink 条目才固定具体 commit。超级项目只跟踪 gitlink，不等于把该 submodule 的许可证全文作为普通文件跟踪；发布前必须在精确 commit 中核验许可证和 notices。
- 用户态的第三方程序不再由本仓库编译。原先 vendored 的 `apps/{git,vim}`、`libs/zlib`、`toolchain/{binutils,musl-cross-make}`、`gcc`、`external/rust` 与 `external/toolchain/Lamina1` 已全部删除，改由 world 清单从 Alpine 上游仓库解析（见 [packaging/overview.md](./packaging/overview.md)）。因此 world 镜像的分发边界由 **Alpine 上游包自身的许可证**决定，不由本仓库的源码树决定。
- 是否需要随附源码、书面 offer、notice、重链接材料或其他内容，取决于实际组件、链接方式和分发形式，不能由“源码可在工作区找到”一概替代。

## 2. 内核与自研代码

| 组件 | 许可证 | 说明 |
|------|--------|------|
| A20OS 项目代码 | 根目录 `LICENSE` 为 Apache-2.0；第三方/来源未决部分除外 | RocketOS-referencing 的 VFS、board 与 GMAC 文件来源和许可证尚未核实，见 [ACKNOWLEDGMENTS.md](./ACKNOWLEDGMENTS.md) |
| A20OS 用户态中由本项目创作且不属于第三方树的部分 | 根目录 `LICENSE` 为 Apache-2.0；仍受逐文件来源审计约束 | liba20c、liba20rt、init、cmds、desktop 等；此行不覆盖 `user/external/` |

## 3. 内核集成的第三方代码

| 组件 | 许可证 | 位置 | 说明 |
|------|--------|------|------|
| lwIP | BSD-3-Clause | `kernel/external/lwip` | 网络协议栈，`NO_SYS=1` 模式集成；跟踪上游 `COPYING` |

## 4. 用户态构建依赖

这些是**从源码编译进 `fat32.img` 根文件系统**的组件。world 镜像里的用户态不在此列。

| 组件 | 许可证 | 位置 |
|------|--------|------|
| musl | MIT | `user/external/musl` |
| mlibc | MIT | `user/external/mlibc` |
| mksh | 逐文件混合：MirBSD/MirOS 条款；`strlcpy.c` 为 ISC；`mbsdcc.h`/`mbsdint.h` 为 CC0 OR MirOS；`expr.c` 含 Unicode notice | `user/external/mksh-cvs2git` |
| sbase | MIT | `user/external/sbase` |
| TLSe | BSD-2-Clause OR Unlicense | `user/external/tlse` |
| fastfetch | MIT | `user/external/apps/fastfetch`（唯一的 gitlink） |

## 5. 用户态程序、静态链接与镜像

基础 `user/Makefile` 使用 `-static`（NOMMU 使用 `-static-pie`）并直接链接 musl CRT 与 `libc.a`；init、mksh、sbase 命令、wget/TLSe 和本地命令等因此包含静态 musl 链接。不能声称这些独立程序“不与 musl 静态链接”。

由源码编译的独立第三方可执行文件只余 fastfetch 一项，且它被并入 FAT32 根而非独立镜像槽。以往 `user/extra.mk` 产出的 git、Vim、GCC、binutils、Rust 与 lamina 已全部删除：GCC/binutils 曾用于自举 riscv64-musl 交叉工具链，Git 曾静态链接源码构建的 zlib 与 curl+mbedtls，lamina 依赖宿主 glibc C++ 工具链动态加载 `laminaCore`。这些能力改由 world 清单从 Alpine 上游解析。

**world 镜像的分发边界**：`build/images/<world>-<arch>.img` 由 `tools/mkrootfs.py` 用 `apk` 从两类来源组装 —— 本仓库的 `a20-*` 包，以及 Alpine 上游仓库的包。因此：

| 组件类别 | 许可证 | 位置 |
|------|--------|------|
| `a20-base` / `a20-min` / `a20-drivers` / `a20-kernel` | 根目录 `LICENSE` 为 Apache-2.0 | `packages/recipes/*.toml`，由本仓库构建产物打包 |
| Alpine 上游包（`devel` 含 vim/git/busybox/curl/less；`devtools` 另含 gcc；`xfce` 含整个 XFCE/Wayland/Mesa 栈；`pynode` 含 python3/nodejs） | **各包自身许可证，含 GPL-2.0-only（vim、git、busybox）与 GPL-3.0（gcc、mesa）** | Alpine 官方仓库，经 `apk` 签名校验后拉取 |

> 分发 `fat32.img`、world 镜像或单个二进制前，应从镜像清单反推其中的精确程序、静态/动态依赖、Alpine 包版本和 gitlink revision，再准备相应许可证、notice 与源码提供材料。Alpine 上游包适用其各自的源码提供义务，`apk` 的包元数据（`.PKGINFO`）应随镜像一并留存。不能假定所有来源都是 submodule，也不能假定只提供超级项目 URL 已满足各组件义务。

## 6. 设计参考与对照系统

A20OS 文档或源码注释提到以下实现参考与研究对照。该分类本身不证明是否存在复制或派生；来源边界见 [ACKNOWLEDGMENTS.md](./ACKNOWLEDGMENTS.md)。

- Uinxed-Kernel（源码注释称 Apache-2.0，但未记录精确 revision）
- RocketOS（许可证与代码来源未决；源码注释称 MIT，但缺精确 revision 或单独授权）
- Windows NT / Zircon (Fuchsia)（设计启发）
- seL4、Capsicum、CHERI、Redox、S3K、Mach（研究对照）

---

_本文件由 A20OS 维护；如有遗漏或错误，请提交 issue/PR。_
