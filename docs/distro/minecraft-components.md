# Minecraft 1.21.11 组件状态

本表记录 Minecraft 1.21.11 在 A20OS 上的各组件运行状态与验证证据。操作入口与实测流程见 [Minecraft](minecraft.md)。

| 组件 | 状态与证据 |
| --- | --- |
| OpenJDK 21 | Alpine musl 21.0.12；默认 ergonomics 估算 1.00 GiB，客户端按自身启动器使用 1536 MiB 上限；未关闭 ImplicitNullChecks |
| 客户端数据 | 1.21.11 client.jar、107 libraries、9 natives、索引 29 与 4591 assets 完整并按清单 SHA1 校验 |
| Classpath | 启动器按冒号分隔逐项重写 library 前缀，最后加入 client.jar |
| LWJGL / GLFW | 实际 Backend library 3.3.3+5；随包 GLFW 同时支持 Wayland/X11，启动器选择原生 Wayland |
| Native ABI | musl JVM + gcompat 加载 LWJGL C JNI；未宣称任意 glibc native 均兼容 |
| OpenAL | 指向 `/usr/lib/libopenal.so.1` musl 库，避免 bundled glibc C++ 异常路径；No Output sound engine 启动通过 |
| ALSA / HDA | 有 control/PCM 节点与原始 ioctl 探针历史记录；实际 alsa-lib 默认设备仍报 Sound protocol is not compatible，尚未完成有声播放 |
| jemalloc | 选择 LWJGL system allocator；没有证明 bundled glibc jemalloc 已修复 |
| Mesa / virgl | 实际 F3 显示 OpenGL 4.3 Core、Mesa 25.2.7、virgl AMD Radeon 780M |
| GPU 共享 | PRIME 真实 VMO、mmap、SCM_RIGHTS 与 exporter 生命周期通过 x86_64/RISC-V 原生测试 |
| GPU 同步 | 真正 host fence 完成后返回；sync_file 已完成 fence 的 poll、合并、详情与依赖测试通过 |
| 桌面合成 | labwc 根据 3D_FEATURES 选择 GLES 或 pixman；Firefox 在两种配置下均完成真实 Bing 验证 |
| 网络 | Minecraft 服务端 HTTP 401 实际返回；SIOCGIFCONF 通用枚举缺陷已修，未把假 token 视为网络故障 |
| 账号与联机 | 官方 demo 作为 QA 入口；未集成 Microsoft 登录，Realms/多人尚未验收 |
| 世界持久化 | 正常 GUI 退出后 gzip 与 sync，通过关机离线读取比对；冷启动世界重载证据见 Minecraft 验证记录 |

不能用“原始 ioctl 返回 0”推断 alsa-lib/OpenAL 整条音频链通过，也不能用 NEEDED 或 dlopen 列表推断 GLFW 最终选择的窗口后端。以真实窗口、F3、玩家交互和保存/重载作为客户端验收依据，音频仍单独保留为待完善项。
