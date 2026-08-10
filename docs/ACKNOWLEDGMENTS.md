# ACKNOWLEDGMENTS

A20OS 使用并参考了许多开源项目与公开标准。本文档集中记录已知的集成、参考和待核实来源；它不对未完成的代码来源审计作“全部自主”或“未复制代码”的保证。

- **直接集成 / 构建的第三方项目**（`user/external/`、`kernel/external/`）：包括普通 tracked tree 和 gitlink submodule，两者必须按各自许可证处理；
- **实现参考与待核实来源**：源码注释记录了部分移植/开发参考，但“参考”一词本身不能证明是否复制、改写或派生；
- **公开标准与规范**：某些格式/协议由标准强制，任何实现都必须遵循。

许可证以普通 tracked tree 中的许可证文本或 gitlink 精确 revision 中的许可证为准；本列表供快速查阅，不替代这些原文。

---

## 1. 直接集成 / 构建的第三方项目

这些项目被构建系统直接消费，或可选地编译/安装进 A20OS 镜像；是否出现在某个具体镜像中取决于 profile 和构建目标。

| 项目 | 用途 | 许可证 | 位置 |
|------|------|--------|------|
| [musl](https://musl.libc.org/) | Linux ABI 用户态 libc | MIT | `user/external/musl` |
| [mlibc](https://github.com/managarm/mlibc) | Native ABI libc 基础 | MIT | `user/external/mlibc` |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) | 内核网络协议栈（`NO_SYS=1`） | BSD-3-Clause | `kernel/external/lwip` |
| [mksh](http://www.mirbsd.org/mksh.htm) | 默认 shell | 逐文件混合：MirBSD/MirOS 条款、ISC、CC0 OR MirOS，并含 Unicode notice | `user/external/mksh-cvs2git` |
| [sbase](https://core.suckless.org/sbase/) | 基础工具（`ls`、`cat` 等） | MIT | `user/external/sbase` |
| [TLSe](https://github.com/eduardsui/tlse) | wget 的 TLS 实现 | BSD-2-Clause OR Unlicense | `user/external/tlse` |
| [LVGL](https://lvgl.io/) | 桌面 GUI 框架 | MIT | `user/external/gui/lvgl` |
| [Weston](https://gitlab.freedesktop.org/wayland/weston) | Wayland compositor | MIT | `user/external/gui/weston` |
| [Wayland](https://gitlab.freedesktop.org/wayland/wayland) | Wayland 协议库 | MIT | `user/external/gui/wayland` |
| [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) | Wayland 协议扩展 | MIT | `user/external/gui/wayland-protocols` |
| [pixman](https://gitlab.freedesktop.org/pixman/pixman) | 像素合成 | MIT | `user/external/gui/pixman` |
| [libxkbcommon](https://github.com/xkbcommon/libxkbcommon) | 键盘布局 | MIT/X11 | `user/external/gui/libxkbcommon` |
| [libevdev](https://gitlab.freedesktop.org/libevdev/libevdev) | evdev 事件库 | MIT | `user/external/gui/libevdev` |
| [libinput](https://gitlab.freedesktop.org/libinput/libinput) | 输入设备库 | MIT | `user/external/gui/libinput` |
| [xkeyboard-config](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config) | XKB 数据 | MIT | `user/external/gui/xkeyboard-config` |
| [libdrm](https://gitlab.freedesktop.org/mesa/drm) | DRM 用户态库 | MIT | `user/external/gui/libdrm` |
| [fastfetch](https://github.com/fastfetch-cli/fastfetch) | 系统信息工具 | MIT | `user/external/apps/fastfetch` |
| [vim](https://github.com/vim/vim) | 编辑器 | Vim License | `user/external/apps/vim` |
| [git](https://github.com/git/git) | 版本控制 | GPL-2.0-only；部分文件使用 GPLv2 兼容的其他许可证 | `user/external/apps/git` |
| [FFmpeg](https://github.com/FFmpeg/FFmpeg) | 媒体编解码 | 默认 LGPL-2.1-or-later；仍须按固定 revision、配置和文件核验 | `user/external/libs/ffmpeg` |
| [binutils](https://www.gnu.org/software/binutils/) | 二进制工具 | GPL-3.0-or-later / LGPL-3.0-or-later 等，按组件文件核验 | `user/external/toolchain/binutils` |
| [Rust 工具链](https://www.rust-lang.org/) | Rust 用户态编译 | 由实际取得的工具链分发确定 | `user/external/rust`（审计基线 `e33c3219` 未跟踪该工具链内容） |
| [musl-cross-make](https://github.com/richfelker/musl-cross-make) | 交叉编译工具链构建 | MIT | `user/external/toolchain/musl-cross-make` |
| [zlib](https://github.com/madler/zlib) | 压缩库 | Zlib | `user/external/libs/zlib` |
| [Breeze](https://invent.kde.org/plasma/breeze) | 桌面图标/主题资源 | 多种 GPL/LGPL 版本，按资源文件核验 | `user/external/gui/breeze` |

> 说明：`git`、`vim`、`binutils`、`ffmpeg` 等项目以独立用户态程序的形式构建，作为 Linux ABI 兼容性的验证负载；它们不参与内核核心的构建。

mksh 不能归并为单一 MirBSD 许可证：多数源文件携带 MirBSD/MirOS 条款，`strlcpy.c` 使用 ISC 条款，`mbsdcc.h` 与 `mbsdint.h` 标注 `CC0 OR The MirOS Licence`，`expr.c` 还嵌入 Unicode 数据 notice。分发时必须保留并按实际文件集合核验这些逐文件 notice。

在 `e33c3219`，`kernel/external/lwip` 以及 `user/external/{musl,mlibc,mksh-cvs2git,sbase,tlse}` 是普通 tracked tree，不是 submodule。`binutils`、`breeze`、`fastfetch`、`ffmpeg`、`git`、`libdrm`、`libevdev`、`libinput`、`libxkbcommon`、`lvgl`、`musl-cross-make`、`pixman`、`vim`、`wayland`、`wayland-protocols`、`weston`、`xkeyboard-config` 和 `zlib` 在 `.gitmodules` 注册，并由超级项目的 gitlink 条目固定 commit。许可证结论必须针对普通树中跟踪的许可证文本，或针对 gitlink 的精确 commit 核验，不能把整个 `user/external/` 一概称为 submodule。

---

## 2. 实现参考与待核实来源

下表只记录源码注释或项目文档声称的参考关系，不证明代码是净室重写，也不解决派生作品或许可证义务。

| 参考项目 | 许可证 | 借鉴内容 | A20OS 中的对应方面 |
|----------|--------|----------|--------------------|
| [Uinxed-Kernel](https://github.com/ViudiraTech/Uinxed-Kernel) | 当前源码注释称 Apache-2.0；精确 revision 未记录 | 当前源码注释归因的 USB BOT/SCSI 传输与 recovery 流程、ISO 9660 名字转小写/去 `;1` 策略 | USB Mass Storage、ISO 9660 |
| [RocketOS](https://gitlab.eduxiji.net/T202510213995926/oskernel2025-rocketos) | **未核实**；当前 A20OS 源码注释称 MIT | VFS 设计启发；VisionFive 2 与 LS2K1000 的 board/GMAC bring-up 参考 | VFS、两个 board 文件与两个 GMAC 驱动；注释没有界定逐行来源 |

RocketOS 边界尚未解决：`kernel/fs/vfs.c` 写有设计启发说明，`kernel/platform/{visionfive2,ls2k1000}/board.c` 和 `kernel/drivers/net/{starfive_gmac,ls2k_gmac}.c` 写有 RocketOS porting/bring-up reference，但仓库没有记录所参考的精确 RocketOS revision、该 revision 的许可证文本、逐文件来源对照或单独授权。在取得这些证据前，不能把注释中的“MIT”、“inspired”或“reference”升级为已验证许可证，也不能断言相关文件未包含复制或改写代码。应对精确 revision 做 provenance/license 审计，或取得覆盖相关代码的单独授权。

---

## 3. 设计思路参考（design inspiration）

以下系统的公开机制被 A20OS 设计文档用作启发或对照，尤其涉及 Native ABI 与安全模型。本节只记录文档层面的关系，不作代码来源审计结论。

| 参考系统 | 启发内容 | A20OS 中的对应方面 |
|----------|----------|--------------------|
| [Microsoft Windows NT](https://learn.microsoft.com/en-us/windows/win32/sysinfo/kernel-objects) | 内核对象 / handle 统一资源模型：以句柄 + 安全权能管理所有内核对象，消灭各类资源的调用差异 | Native ABI 的 handle/capability 体系 |
| [Zircon (Fuchsia)](https://fuchsia.dev/fuchsia-src/concepts/kernel) | Channel handle transfer 语义、futex 定位、ABI 版本管理对照 | Native ABI 的 Channel/Event 队列与 futex |

> 注意：Zircon、Windows NT 既是设计参考，也是研究文档中的**对照分析对象**（见下节），两者视角不同："参考"指启发了设计，"对照"指作为比较基线分析优劣。

---

## 4. 研究对照系统（analysis objects）

`docs/research/` 系列文档在论证 A20OS 的设计时，将以下系统作为**对比/分析对象**——它们不是 A20OS 的代码或设计来源，而是用于评估 A20OS 特性（时态能力、混合信任边界、内核层 session type、ABI 版本化、委托模式等）的学术基线。

| 系统 | 对照内容 |
|------|----------|
| Zircon (Fuchsia) | 能力系统、Channel 语义、syscall 版本化 |
| [seL4](https://sel4.systems/) | 能力系统形式化、CSpace 撤销、Endpoint 无类型约束 |
| [Capsicum](https://www.freebsd.org/cgi/man.cgi?query=capsicum) | 混合信任边界、能力纪律 |
| [CHERI](https://www.cl.cam.ac.uk/research/security/ctsrd/cheri/) | 硬件 capability、无硬件支持对比 |
| [Redox](https://www.redox-os.org/) | 能力内核对照 |
| S3K | 能力系统对照 |
| [Mach](https://en.wikipedia.org/wiki/Mach_(kernel)) | 微内核 IPC 对照 |

> 与 §2「实现参考」不同，当前文档只把本节项目列为学术比较基线，没有把它们登记为代码实现来源；这种分类本身不是完整 provenance 审计，也不能用来证明从未受其实现影响。

---

## 5. 公开标准与规范

以下格式/协议主要受公开标准约束，因此单列为规范来源。列在本节不能替代具体源码的 provenance 审计，也不自动解决派生作品判断：

- **ISO 9660**（CD-ROM 文件系统）— ECMA-119 / ISO 9660
- **USB Bulk-Only Transport (BOT)** — USB Implementers Forum, Mass Storage Class Bulk-Only Transport spec
- **SCSI 命令集**（READ(10)/WRITE(10)/READ CAPACITY(10)/INQUIRY）— SCSI Primary / Block Commands (SPC/SBC)
- **TPM 2.0 / TIS / CRB** — Trusted Computing Group (TCG) PC Client Platform TPM Profile
- **Linux ABI / 系统调用约定** — 兼容层照搬 Linux 用户态 ABI 的编号与语义
- **POSIX** — 用户态 API 语义遵循 POSIX

---

## 6. 我们致谢的开源社区与平台

- **系统软件（系统软件��作系统设计赛）**——提供交流平台、测试环境与方向指引。
- **OSDev Wiki**——无数 OS 开发者的公共知识库。
- 所有上述第三方项目的维护者与贡献者。

---

_本文件为致谢与出处说明，不替代任何第三方项目的原始 LICENSE。若您认为有遗漏或错误，欢迎提交 issue / PR。_
