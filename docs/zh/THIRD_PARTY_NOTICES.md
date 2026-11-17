# THIRD-PARTY NOTICES

本文件索引 A20OS 源码与构建产物可能包含的第三方组件。它不是法律意见，也不能替代针对实际分发物、精确源码 revision 和对应 LICENSE/COPYING 的核验。

## 1. 分发边界声明

A20OS 仓库不能预先限定下游只分发源码。`fat32.img`、`ext4.img`、world 镜像（`build/images/*.img`）、ISO、独立可执行文件和共享库一旦提供给第三方，就是需要按其实际内容审查的二进制/资源分发物。因此：

- `kernel/external/{lwip,uacpi}` 是普通 tracked tree；`user/external/{musl,mlibc}` 与 fastfetch 是固定 commit 的 submodule。
- `.gitmodules` 登记用户态 submodule 的来源，包括可选的 `user/external/rtw89`；超级项目的 gitlink 条目固定具体 commit。超级项目只跟踪 gitlink，不等于把该 submodule 的许可证全文作为普通文件跟踪；发布前必须在精确 commit 中核验许可证和 notices。
- BusyBox、fastfetch 和可选 rtw89d 仍从源码构建；其他大体量第三方应用交由 world。原先 vendored 的 `apps/{git,vim}`、`libs/zlib`、`toolchain/{binutils,musl-cross-make}`、`gcc`、`external/rust` 与 `external/toolchain/Lamina1` 已全部删除，改由 world 清单从 Alpine 上游仓库解析（见 [packaging/overview.md](packaging/overview.md)）。因此 world 镜像的分发边界由 **Alpine 上游包自身的许可证**决定，不由本仓库的源码树决定。
- 是否需要随附源码、书面 offer、notice、重链接材料或其他内容，取决于实际组件、链接方式和分发形式，不能由“源码可在工作区找到”一概替代。

## 2. 内核与自研代码

| 组件 | 许可证 | 说明 |
|------|--------|------|
| A20OS 项目代码 | 根目录 `LICENSE` 为 Apache-2.0；第三方/来源未决部分除外 | RocketOS-referencing 的 VFS、board 与 GMAC 文件来源和许可证尚未核实，见 [ACKNOWLEDGMENTS.md](ACKNOWLEDGMENTS.md) |
| A20OS 用户态中由本项目创作且不属于第三方树的部分 | 根目录 `LICENSE` 为 Apache-2.0；仍受逐文件来源审计约束 | liba20c、liba20rt、init、cmds、desktop 等；此行不覆盖 `user/external/` |

## 3. 内核集成的第三方代码

| 组件 | 许可证 | 位置 | 说明 |
|------|--------|------|------|
| uACPI | MIT | `kernel/external/uacpi` | 7.0.0；上游版本与归档校验值见 `A20OS-SOURCE`，分发保留 `LICENSE` |
| lwIP | BSD-3-Clause | `kernel/external/lwip` | 网络协议栈，`NO_SYS=1` 模式集成；跟踪上游 `COPYING` |

## 4. 用户态构建依赖

这些是**从源码编译进 `fat32.img` 根文件系统**的组件。world 镜像里的用户态不在此列。

| 组件 | 许可证 | 位置 |
|------|--------|------|
| musl | MIT | `user/external/musl` |
| BusyBox 静态用户态（精简 RAMFS） | GPL-2.0-only；静态链接的 musl 为 MIT | Alpine `busybox-static`；版本、架构 SHA-256、源码配方见 `tools/packaging/busybox.lock.json`，`.PKGINFO` 留存在 `build/cache/busybox/<arch>/busybox.PKGINFO`。ELF 作为独立用户程序嵌入 RAMFS，发布仍须准备对应源码、补丁、配置和许可证材料 |
| BusyBox 源码用户态 | GPL-2.0-only；静态链接 musl 为 MIT | 上游 1.37.0 归档与 SHA-256 见 `tools/packaging/build_busybox.py`；生成目录 `.busybox.config` 和 `.busybox.source.json` 保存构建材料 |
| PPP CHAP-MD5 | Public Domain（源码声明基于 RFC1321 / LibTomCrypt） | `user/crypto/md5.c`；来源及最小修改见同目录 README |
| mlibc | MIT | `user/external/mlibc` |
| fastfetch | MIT | `user/external/apps/fastfetch`（gitlink） |
| rtw89d / AirPort_RTW89 | 含 GPL-2.0；Realtek 驱动部分为 GPL-2.0 OR BSD-3-Clause；其余依赖见上游 notices | `user/external/rtw89`，独立用户态可执行文件；A20 适配 `user/wifi` 为 BSD-3-Clause |
| RTL8852B 固件 | Realtek 固件再分发条款 | `rtw8852b_fw-2.bin`，固定 hash；条款随 `/a20/rtw89-notices` 提供 |

mksh 与 sbase 已从生产依赖及 submodule 移除。仅显式 Native ABI 测试按固定
提交获取到 `build/cache/test-sources`；前者保留逐文件 MirOS/ISC/CC0/Unicode
条款，后者为 MIT。mksh 配置头保留在 `tools/test-fixtures/mksh`，含原始 notices。
TLSe 不再参与构建或镜像。

## 5. 用户态程序、静态链接与镜像

基础 `user/Makefile` 使用 `-static`（NOMMU 使用 `-static-pie`）并直接链接 musl CRT 与 `libc.a`；平台 init、源码 BusyBox、fastfetch 和本地命令等因此包含静态 musl 链接。这些独立程序均包含与 musl 的静态链接。

由源码编译的独立第三方可执行文件包括 BusyBox、fastfetch，以及笔记本实例可选的 `rtw89d`。
后者包含无线 station/crypto 和固件，独立于内核链接，详情见
[RTL8852BE 无线网卡](drivers/classes/rtl8852be-wifi.md)。以往 `user/extra.mk` 产出的 git、Vim、GCC、binutils、Rust 与 lamina 已全部删除：GCC/binutils 曾用于自举 riscv64-musl 交叉工具链，Git 曾静态链接源码构建的 zlib 与 curl+mbedtls，lamina 依赖宿主 glibc C++ 工具链动态加载 `laminaCore`。这些能力改由 world 清单从 Alpine 上游解析。

**world 镜像的分发边界**：`build/images/<world>-<arch>.img` 由 `tools/packaging/mkrootfs.py` 用 `apk` 从两类来源组装 —— 本仓库的 `a20-*` 包，以及 Alpine 上游仓库的包。因此：

| 组件类别 | 许可证 | 位置 |
|------|--------|------|
| `a20-base` / `a20-min` | Apache-2.0 AND GPL-2.0-only AND MIT（按实际文件适用） | `packages/recipes/*.toml`，由本仓库构建产物打包 |
| Alpine 上游包（`devel` 含 vim/git/busybox/curl/less；`devtools` 另含 gcc；`xfce` 含整个 XFCE/Wayland/Mesa 栈；`pynode` 含 python3/nodejs） | **各包自身许可证，含 GPL-2.0-only（vim、git、busybox）与 GPL-3.0（gcc、mesa）** | Alpine 官方仓库，经 `apk` 签名校验后拉取 |

> 分发 `fat32.img`、world 镜像或单个二进制前，应从镜像清单反推其中的精确程序、静态/动态依赖、Alpine 包版本和 gitlink revision，再准备相应许可证、notice 与源码提供材料。Alpine 上游包适用其各自的源码提供义务，`apk` 的包元数据（`.PKGINFO`）应随镜像一并留存。不能假定所有来源都是 submodule，也不能假定只提供超级项目 URL 已满足各组件义务。

## 6. 设计参考与对照系统

A20OS 文档或源码注释提到以下实现参考与研究对照。该分类本身不证明是否存在复制或派生；来源边界见 [ACKNOWLEDGMENTS.md](ACKNOWLEDGMENTS.md)。

- Uinxed-Kernel（源码注释称 Apache-2.0，但未记录精确 revision）
- RocketOS（许可证与代码来源未决；源码注释称 MIT，但缺精确 revision 或单独授权）
- Windows NT / Zircon (Fuchsia)（设计启发）
- seL4、Capsicum、CHERI、Redox、S3K、Mach（研究对照）

---

_本文件由 A20OS 维护；如有遗漏或错误，请提交 issue/PR。_

## LLVM 编译器运行库

选择 `TOOLCHAIN=llvm` 时，构建系统从官方校验归档编译 compiler-rt 19.1.7 builtins，替代 libgcc。源码及配套 CMake 模块保存在 `build/toolchains/sources/`，采用 Apache-2.0 WITH LLVM-exception（以各自源码 LICENSE.TXT 为准）。归档校验值、构建步骤和运行库 ABI 说明见 [LLVM 工具链](dev/llvm-toolchain.md)。默认 GCC 构建路径仍使用其对应运行库及许可。
