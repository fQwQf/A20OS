# Minecraft 1.21.11 组件状态

本表记录 Minecraft 1.21.11 在 A20OS 上的各组件运行状态与验证证据。操作入口与实测流程见 [Minecraft](minecraft.md)。

| 组件 | 状态与证据 |
| --- | --- |
| OpenJDK 21 | 本轮 x86_64 为 Alpine musl 21.0.13.0.3，JVM 应用分项见[应用证据](../kernel/linux-abi/application-contracts.md#x86_64-桌面与-jvm-的直接运行2026-11-16)；历史 21.0.12 的默认 ergonomics 估算 1.00 GiB，客户端按自身启动器使用 1536 MiB 上限；未关闭 ImplicitNullChecks |
| 客户端数据 | 1.21.11 client.jar、107 libraries、9 natives；本轮 x86_64 索引 29 的 4593 个资源条目已按下载清单 SHA1 校验，数量以该次实际清单为准 |
| Classpath | 启动器按冒号分隔逐项重写 library 前缀，最后加入 client.jar |
| LWJGL / GLFW | 实际 Backend library 3.3.3+5；随包 GLFW 同时支持 Wayland/X11，启动器选择原生 Wayland |
| Native ABI | musl JVM + gcompat 加载 LWJGL C JNI；已恢复失效的 `libdl.so.2 -> libc.so.6` 链接，本轮 Java 直接加载 LWJGL/GLFW 通过；未宣称任意 glibc native 均兼容 |
| OpenAL | 指向 `/usr/lib/libopenal.so.1` musl 库，避免 bundled glibc C++ 异常路径；本轮两次 ALSA Default sound engine 启动通过 |
| ALSA / HDA | 本轮游戏经 ALSA/HDA 写入 QEMU WAV；两次 44.1 kHz 双声道录音非静音，峰值 20677/13108；仅证明 QEMU 播放路径，不认证宿主采集或物理扬声器 |
| jemalloc | 选择 LWJGL system allocator；没有证明 bundled glibc jemalloc 已修复 |
| Mesa / virgl | 实际 F3 显示 OpenGL 4.3 Core、Mesa 25.2.7、virgl AMD Radeon 780M |
| GPU 共享 | PRIME 真实 VMO、mmap、SCM_RIGHTS 与 exporter 生命周期通过 x86_64/RISC-V 原生测试 |
| GPU 同步 | 真正 host fence 完成后返回；sync_file 已完成 fence 的 poll、合并、详情与依赖测试通过 |
| 桌面合成 | labwc 根据 3D_FEATURES 选择 GLES 或 pixman；Firefox 在两种配置下均完成真实 Bing 验证 |
| 网络 | Minecraft 服务端 HTTP 401 实际返回；SIOCGIFCONF 通用枚举缺陷已修，未把假 token 视为网络故障 |
| 账号与联机 | 官方 demo 作为 QA 入口；未集成 Microsoft 登录，Realms/多人尚未验收 |
| 世界持久化 | 本轮 x86_64 两次正常 GUI 保存/退出、关机后 gzip/hash/NBT 与同盘冷启动实际重载通过；位置与朝向证据见下方记录 |

不能用“原始 ioctl 返回 0”推断 alsa-lib/OpenAL 整条音频链通过，也不能用 NEEDED 或 dlopen 列表推断 GLFW 最终选择的窗口后端。以真实窗口、F3、玩家交互和保存/重载作为客户端验收依据，本轮 QEMU 播放已有直接证据，采集与实机范围仍按各自契约披露。

本轮直接证据为 `build/evidence/v1-closeout-20261116/desktop-current/minecraft-contract.json`，
链接到两次运行报告、F3/背包画面、离线 NBT 和音频检查；详细范围见
[应用契约](../kernel/linux-abi/application-contracts.md#x86_64-桌面与-jvm-的直接运行2026-11-16)。
