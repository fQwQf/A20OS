# THIRD-PARTY NOTICES

本文件索引 A20OS 源码与构建产物可能包含的第三方组件。它不是法律意见，也不能替代针对实际分发物、精确源码 revision 和对应 LICENSE/COPYING 的核验。

## 1. 分发边界声明

A20OS 仓库不能预先限定下游只分发源码。`fat32.img`、`ext4.img`、`extra.img`、ISO、独立可执行文件和共享库一旦提供给第三方，就是需要按其实际内容审查的二进制/资源分发物。因此：

- `kernel/external/lwip` 和 `user/external/{musl,mlibc,mksh-cvs2git,sbase,tlse}` 是普通 tracked tree；不能称为 submodule。
- `.gitmodules` 登记外部项目的路径和 URL，超级项目的 gitlink 条目才固定具体 commit。超级项目只跟踪 gitlink，不等于把每个 submodule 的许可证全文作为普通文件跟踪；发布前必须在精确 commit 中核验许可证和 notices。
- `user/external/rust` 与 `user/external/riscv64-glibc-sysroot` 的本地内容未被仓库跟踪（2026-08 核实时）；若把由它们产生或取得的文件装入镜像，必须另行保留来源、版本和许可证材料。
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

| 组件 | 许可证 | 位置 |
|------|--------|------|
| musl | MIT | `user/external/musl` |
| mlibc | MIT | `user/external/mlibc` |
| mksh | 逐文件混合：MirBSD/MirOS 条款；`strlcpy.c` 为 ISC；`mbsdcc.h`/`mbsdint.h` 为 CC0 OR MirOS；`expr.c` 含 Unicode notice | `user/external/mksh-cvs2git` |
| sbase | MIT | `user/external/sbase` |
| TLSe | BSD-2-Clause OR Unlicense | `user/external/tlse` |
| LVGL | MIT | `user/external/gui/lvgl` |
| Weston | MIT | `user/external/gui/weston` |
| Wayland | MIT | `user/external/gui/wayland` |
| wayland-protocols | MIT | `user/external/gui/wayland-protocols` |
| pixman | MIT | `user/external/gui/pixman` |
| libxkbcommon | MIT/X11 | `user/external/gui/libxkbcommon` |
| libevdev | MIT | `user/external/gui/libevdev` |
| libinput | MIT | `user/external/gui/libinput` |
| xkeyboard-config | MIT | `user/external/gui/xkeyboard-config` |
| libdrm | MIT | `user/external/gui/libdrm` |
| fastfetch | MIT | `user/external/apps/fastfetch` |
| zlib | Zlib | `user/external/libs/zlib` |
| musl-cross-make | MIT | `user/external/toolchain/musl-cross-make` |
| Rust 工具链 | 必须按实际取得的分发核验 | `user/external/rust`（2026-08 核实时仓库未跟踪内容） |

## 5. 用户态程序、静态链接与镜像

基础 `user/Makefile` 使用 `-static`（NOMMU 使用 `-static-pie`）并直接链接 musl CRT 与 `libc.a`；init、mksh、sbase 命令、wget/TLSe 和本地命令等因此包含静态 musl 链接。`user/extra.mk` 同样设置 `-static`，Vim、Git 及其辅助库也链接 musl CRT/`libc.a`。不能声称这些独立程序“不与 musl 静态链接”。Wayland 构建则同时产生共享库，FFmpeg 配置为 shared。

| 组件 | 许可证 | 位置 | 备注 |
|------|--------|------|------|
| git | GPL-2.0-only | `user/external/apps/git` | 独立可执行文件 |
| vim | Vim License | `user/external/apps/vim` | 独立可执行文件 |
| GCC | 应按实际取得源码的许可证与 GCC Runtime Library Exception 核验 | `user/external/gcc`（2026-08 核实时仓库未跟踪该目录） | `user/extra.mk` 仅在 `configure` 存在时启用可选工具链构建 |
| binutils | GPL-3.0 / LGPL-3.0 | `user/external/toolchain/binutils` | 独立工具链 |
| lamina (Lamina1) | 根目录暂无 LICENSE 文本，按实际取得源码核验；子模块 LMCAS/LAMMP 为 LGPL-2.1，dyncall 为逐文件 BSD 风格 | `user/external/toolchain/Lamina1` | 独立可执行文件 + 4 个共享库（laminaCore/lmcas/lmmc/LammpCore，含版本化 SONAME 文件）与 libstdc++.so.6，动态链接 glibc（运行库与 rust 包共用） |
| FFmpeg | LGPL-2.1-or-later（以固定 revision 配置为准） | `user/external/libs/ffmpeg` | `user/wayland/build.sh` 构建共享库，非 `user/extra.mk` 独立程序 |
| Breeze | 多种 GPL/LGPL 版本，按资源文件核验 | `user/external/gui/breeze` | 资源/图标 |

> 分发 `fat32.img`、`extra.img`、GUI 镜像或单个二进制前，应从镜像清单反推其中的精确程序、静态/动态依赖和 gitlink revision，再准备相应许可证、notice 与源码提供材料。不能假定所有来源都是 submodule，也不能假定只提供超级项目 URL 已满足各组件义务。

## 6. 设计参考与对照系统

A20OS 文档或源码注释提到以下实现参考与研究对照。该分类本身不证明是否存在复制或派生；来源边界见 [ACKNOWLEDGMENTS.md](./ACKNOWLEDGMENTS.md)。

- Uinxed-Kernel（源码注释称 Apache-2.0，但未记录精确 revision）
- RocketOS（许可证与代码来源未决；源码注释称 MIT，但缺精确 revision 或单独授权）
- Windows NT / Zircon (Fuchsia)（设计启发）
- seL4、Capsicum、CHERI、Redox、S3K、Mach（研究对照）

---

_本文件由 A20OS 维护；如有遗漏或错误，请提交 issue/PR。_
