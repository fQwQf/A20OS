# THIRD-PARTY NOTICES

本文件集中声明 A20OS 分发包中涉及的第三方组件及其许可证，作为对[LICENSE](../LICENSE)（Apache-2.0）第 4(c)/4(d) 条的落实。许可证全文以各组件仓库内的 LICENSE/COPYING 文件为准；本文件仅作索引与再分发声明。

## 1. 分发边界声明

A20OS 的分发物是**源代码**（本仓库，含 git submodule）。所有镜像（`fat32.img`、`ext4.img`、`extra.img`、ISO 等）均为**由本仓库源码与submodule 源码构建出来的构建产物**，不作为独立的二进制分发包对外分发。因此：

- GPL/LGPL 组件（git、vim、gcc、binutils、FFmpeg、Breeze 等）以**源码形式** 随仓库的 `user/external/` submodule 提供，构建者在本地自行编译。
- 构建产物如需分发给他人，应连同对应组件的源码获取说明一起提供 （各 GPL 组件的源码即其 submodule）。

## 2. 内核与自研代码

| 组件 | 许可证 | 说明 |
|------|--------|------|
| A20OS 内核（`kernel/`） | Apache-2.0 | 自研；含对 Uinxed-Kernel (Apache-2.0)、RocketOS (MIT) 等的设计借鉴，详见 [ACKNOWLEDGMENTS.md](./ACKNOWLEDGMENTS.md) |
| A20OS 用户态（`user/` 自研部分） | Apache-2.0 | liba20c、liba20rt、init、cmds、desktop 等 |

## 3. 内核集成的第三方代码

| 组件 | 许可证 | 位置 | 说明 |
|------|--------|------|------|
| lwIP | BSD-3-Clause | `kernel/external/lwip` | 网络协议栈，`NO_SYS=1` 模式集成；保留上游 COPYING |

## 4. 用户态构建依赖（构建时引入，源码随 submodule）

| 组件 | 许可证 | 位置 |
|------|--------|------|
| musl | MIT | `user/external/musl` |
| mlibc | MIT | `user/external/mlibc` |
| mksh | MirBSD License | `user/external/mksh-cvs2git` |
| sbase | MIT | `user/external/sbase` |
| tlse | MIT（可选双许可） | `user/external/tlse` |
| LVGL | MIT | `user/external/lvgl` |
| Weston | MIT | `user/external/weston` |
| Wayland | MIT | `user/external/wayland` |
| wayland-protocols | MIT | `user/external/wayland-protocols` |
| pixman | MIT | `user/external/pixman` |
| libxkbcommon | MIT/X11 | `user/external/libxkbcommon` |
| libevdev | MIT | `user/external/libevdev` |
| libinput | MIT | `user/external/libinput` |
| xkeyboard-config | MIT | `user/external/xkeyboard-config` |
| libdrm | MIT | `user/external/libdrm` |
| fastfetch | MIT | `user/external/fastfetch` |
| zlib | Zlib | `user/external/zlib` |
| musl-cross-make | MIT | `user/external/musl-cross-make` |
| Rust 工具链 | MIT/Apache-2.0 | `user/external/rust` |

## 5. 独立用户态程序（extra 构建，源码随 submodule，产物不随 A20OS 分发）

以下组件以**独立可执行程序**形式由 `user/extra.mk` 构建（GPL/LGPL 组件的"独立且分离"原则适用；它们不链接进内核，也不与 musl 静态链接成单一二进制）：

| 组件 | 许可证 | 位置 | 备注 |
|------|--------|------|------|
| git | GPL-2.0-only | `user/external/git` | 独立可执行文件 |
| vim | Vim License | `user/external/vim` | 独立可执行文件 |
| gcc | GPL-3.0-with-GCC-exception | `user/external/`（gcc 源码） | 独立工具链 |
| binutils | GPL-3.0 / LGPL-3.0 | `user/external/binutils` | 独立工具链 |
| FFmpeg | LGPL-2.1-or-later | `user/external/ffmpeg` | 独立可执行文件 |
| Breeze | GPL-2.0 / LGPL | `user/external/breeze` | 资源/图标 |

> 若将上述任一构建产物（extra.img 等）分发给第三方，请按对应 GPL/LGPL 条款随附其源码（即 `user/external/` 下对应 submodule）与许可证文本。

## 6. 设计参考与对照系统

A20OS 借鉴/参照了以下项目的设计或将其作为研究对照，但**不使用其代码**；许可证声明见 [ACKNOWLEDGMENTS.md](./ACKNOWLEDGMENTS.md)。

- Uinxed-Kernel（Apache-2.0）
- RocketOS（MIT）
- Windows NT / Zircon (Fuchsia)（设计启发）
- seL4、Capsicum、CHERI、Redox、S3K、Mach（研究对照）

---

_本文件由 A20OS 维护；如有遗漏或错误，请提交 issue/PR。_
