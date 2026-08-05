# ACKNOWLEDGMENTS

A20OS 是自主编写的操作系统，但站在了许多优秀开源项目与公开标准的肩膀上。本文档集中记录：

- **直接集成 / 构建的第三方项目**（`user/external/`、`kernel/external/`）：它们保留了各自的许可证，作为外部依赖被引入、编译并链接；
- **作为实现参考（inspired-by）的项目**：A20OS 借鉴了其设计思路或实现流程，但代码是为 A20OS 自己的 VFS / 块设备 / 驱动模型重写的；
- **公开标准与规范**：某些格式/协议由标准强制，任何实现都必须遵循。

许可证以各项目仓库为准；本列表供快速查阅，不替代各自的 LICENSE 文件。

---

## 1. 直接集成 / 构建的第三方项目

这些项目以原样或经适配的形式编译进 A20OS 的镜像，各自携带独立许可证。

| 项目 | 用途 | 许可证 | 位置 |
|------|------|--------|------|
| [musl](https://musl.libc.org/) | Linux ABI 用户态 libc | MIT | `user/external/musl` |
| [mlibc](https://github.com/managarm/mlibc) | Native ABI libc 基础 | MIT | `user/external/mlibc` |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) | 内核网络协议栈（`NO_SYS=1`） | BSD-3-Clause | `kernel/external/lwip` |
| [mksh](http://www.mirbsd.org/mksh.htm) | 默认 shell | MirBSD License | `user/external/mksh-cvs2git` |
| [sbase](https://core.suckless.org/sbase/) | 基础工具（`ls`、`cat` 等） | MIT | `user/external/sbase` |
| [tlse](https://github.com/eduardsui/tlse) | wget 的 TLS 实现 | MIT | `user/external/tlse` |
| [LVGL](https://lvgl.io/) | 桌面 GUI 框架 | MIT | `user/external/lvgl` |
| [Weston](https://gitlab.freedesktop.org/wayland/weston) | Wayland compositor | MIT | `user/external/weston` |
| [Wayland](https://gitlab.freedesktop.org/wayland/wayland) | Wayland 协议库 | MIT | `user/external/wayland` |
| [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) | Wayland 协议扩展 | MIT | `user/external/wayland-protocols` |
| [pixman](https://gitlab.freedesktop.org/pixman/pixman) | 像素合成 | MIT | `user/external/pixman` |
| [libxkbcommon](https://github.com/xkbcommon/libxkbcommon) | 键盘布局 | MIT | `user/external/libxkbcommon` |
| [libevdev](https://gitlab.freedesktop.org/libevdev/libevdev) | evdev 事件库 | MIT | `user/external/libevdev` |
| [libinput](https://gitlab.freedesktop.org/libinput/libinput) | 输入设备库 | MIT | `user/external/libinput` |
| [xkeyboard-config](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config) | XKB 数据 | MIT | `user/external/xkeyboard-config` |
| [libdrm](https://gitlab.freedesktop.org/mesa/drm) | DRM 用户态库 | MIT | `user/external/libdrm` |
| [fastfetch](https://github.com/fastfetch-cli/fastfetch) | 系统信息工具 | MIT | `user/external/fastfetch` |
| [vim](https://github.com/vim/vim) | 编辑器 | Vim License | `user/external/vim` |
| [git](https://github.com/git/git) | 版本控制 | GPL-2.0 | `user/external/git` |
| [FFmpeg](https://github.com/FFmpeg/FFmpeg) | 媒体编解码 | LGPL-2.1+ | `user/external/ffmpeg` |
| [binutils](https://www.gnu.org/software/binutils/) | 二进制工具 | GPL-3.0 | `user/external/binutils` |
| [Rust 工具链](https://www.rust-lang.org/) | Rust 用户态编译 | MIT/Apache-2.0 | `user/external/rust` |
| [musl-cross-make](https://github.com/richfelker/musl-cross-make) | 交叉编译工具链构建 | MIT | `user/external/musl-cross-make` |
| [zlib](https://github.com/madler/zlib) | 压缩库 | Zlib | `user/external/zlib` |
| [Breeze](https://invent.kde.org/plasma/breeze) | 桌面图标/主题资源 | GPL-2.0 / LGPL | `user/external/breeze` |

> 说明：`git`、`vim`、`binutils`、`ffmpeg` 等项目以独立用户态程序的形式构建，作为 Linux ABI 兼容性的验证负载；它们不参与内核核心的构建。

---

## 2. 作为实现参考（inspired-by）的项目

这些项目的代码没有直接进入 A20OS 源码树，但其**设计思路、算法或实现流程**被借鉴。A20OS 的相关实现是为自身架构重写的，并尽量遵守参考项目的许可证要求（本文档即属"注明修改"）。

| 参考项目 | 许可证 | 借鉴内容 | A20OS 中的对应方面 |
|----------|--------|----------|--------------------|
| [Uinxed-Kernel](https://github.com/ViudiraTech/Uinxed-Kernel) | Apache-2.0 | USB BOT/SCSI 传输流程（CBW→data→CSW→reset）、TIS FIFO 状态机、ISO9660 名字转小写策略、ext4 部分截断的 collect-rebuild 思路 | USB Mass Storage、TPM 2.0、ISO9660、ext4 |
| [RocketOS](https://gitlab.eduxiji.net/T202510213995926/oskernel2025-rocketos) | MIT | VFS 层设计；以及 星光 2（StarFive VisionFive 2）与龙芯 2K1000（Loongson LS2K1000）的移植参考，据此开发了对应板级驱动 | VFS、VisionFive 2 与 LS2K1000 板级驱动（SDIO、VirtIO-blk、GMAC） |

> 以上"参考"项目均指**借鉴设计/流程**，A20OS 的实现是为自身模型重写的，未包含其源码。

---

## 3. 设计思路参考（design inspiration）

以下系统的**设计思路**启发了 A20OS 的架构，尤其是 Native ABI 与安全模型；A20OS 没有使用它们的代码，也没有参考（有可能不可能）其源码，但设计文档中明确引用了其机制作为对照。

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

> 与 §2「实现参考」不同，本节项目**没有影响 A20OS 的代码实现**，仅在研究文档中作为学术比较基线出现，不构成"参考实现"或"借鉴"。

---

## 5. 公开标准与规范

以下格式/协议由公开标准强制，任何实现都必须遵循其布局，不构成对某个项目的派生：

- **ISO 9660**（CD-ROM 文件系统）— ECMA-119 / ISO 9660
- **USB Bulk-Only Transport (BOT)** — USB Implementers Forum, Mass Storage Class Bulk-Only Transport spec
- **SCSI 命令集**（READ(10)/WRITE(10)/READ CAPACITY(10)/INQUIRY）— SCSI Primary / Block Commands (SPC/SBC)
- **TPM 2.0 / TIS / CRB** — Trusted Computing Group (TCG) PC Client Platform TPM Profile
- **Linux ABI / 系统调用约定** — 兼容层照搬 Linux 用户态 ABI 的编号与语义
- **POSIX** — 用户态 API 语义遵循 POSIX

---

## 6. 我们致谢的开源社区与平台

- **全国大学生操作系统大赛（全国大学生计算机系统能力大赛·操作系统设计赛）**——提供交流平台、测试环境与方向指引。
- **OSDev Wiki**——无数 OS 开发者的公共知识库。
- 所有上述第三方项目的维护者与贡献者。

---

_本文件为致谢与出处说明，不替代任何第三方项目的原始 LICENSE。若您认为有遗漏或错误，欢迎提交 issue / PR。_
