# Minecraft Java 客户端

A20OS 的 XFCE 镜像可在 x86_64、aarch64 与 riscv64 上运行 Minecraft Java
1.21.11，使用 OpenJDK 21、原生 Wayland 窗口和 virgl OpenGL。默认桌面入口启动官方 demo，便于在没有登录会话
的测试镜像中验证世界生成和交互。账号登录、Realms 和联机尚未验收。

## 构建和启动

宿主机需要 QEMU virgl 支持、可用的 OpenGL 显示或 EGL render node。测试使用
q35/KVM、4 CPU、4 GiB 和 AMD Radeon 780M；普通 2D virtio GPU 仍可运行 XFCE
与 Firefox，但不建议用于 Minecraft。

```sh
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11
tools/a20 check minecraft-x86_64
tools/a20 run minecraft-x86_64 -- GUI_MEDIA=build/minecraft/1.21.11
```

`instances/minecraft-x86_64.toml` 使用 4 GiB 内存、4 GiB XFCE world 和 virgl
GPU。下载脚本读取 Mojang 版本清单并按 SHA1 校验客户端、library 和资源。
本次完整数据为 107 个 library、9 个 native、4591 个 asset 对象，资源索引 29。
镜像内文件位于 `/usr/share/a20-media/1.21.11`；客户端数据不会提交进 Git。

在已经启动的桌面中，点击 Minecraft 菜单入口或运行：

```sh
minecraft --demo
GAMEDIR="$HOME/.minecraft-test" minecraft --demo --width 854 --height 480
MC_MIN_HEAP=256m MC_MAX_HEAP=1536m minecraft --demo
```

启动器使用桌面已有的 `WAYLAND_DISPLAY` 与 `XDG_RUNTIME_DIR`，清除本进程的
`DISPLAY` 以选择 GLFW Wayland 后端。它为新游戏目录复制版本匹配的初始配置：
Custom、4 区块视距、5 区块模拟距离、关闭云和 mipmap。已有玩家配置不会覆盖。
Java wrapper 只定位真实 JVM，不再全局注入堆上限或关闭隐式空指针检查；
Minecraft 的堆设置局限于自身启动器。

## 多架构客户端（aarch64 / riscv64）

Minecraft Java 1.21.11 固定使用 LWJGL 3.3.3；Mojang 只发布 x86_64 的
Linux native，因此其他架构按下表选择 LWJGL 来源。loongarch64 目前在任何
发行渠道都没有 LWJGL native，下载脚本会直接报错退出，不会产出不完整的
客户端目录。

| 架构 | LWJGL 版本 | 来源 | 说明 |
| ---- | ---------- | ---- | ---- |
| x86_64 | 3.3.3 | Mojang 版本清单 | 原有路径，模块 jar 与 native 均随清单下载 |
| aarch64 | 3.3.3 | Maven Central | `natives-linux-arm64`，与 Mojang 固定版本一致 |
| riscv64 | 3.3.6 | Maven Central | riscv64 native 自 3.3.6 起发布；8 个模块必须整套同版本，加载器在装载时强制一致 |

```sh
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11-x86_64  x86_64
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11-aarch64 aarch64
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11-riscv64 riscv64
```

Maven 路径会跳过版本清单中的全部 org/lwjgl 条目（普通 jar 与 native jar
都由 Maven 版本取代），下载后校验 SHA1，解包 native 时校验 ELF magic 与
e_machine（aarch64=183，riscv64=243），避免混入宿主或错误架构的 .so。
client.jar、其余 library 与 4591 个 asset 对象三架构一致；aarch64 /
riscv64 的 classpath 为 67 个 jar（51 个原版 + 16 个 LWJGL），解包 8 个
LWJGL native .so。

对应实例 `instances/minecraft-{aarch64,riscv64}.toml` 与 x86_64 相同：
4 GiB 内存、4 GiB XFCE world、virgl GPU；aarch64 因 SMP 竞态固定单 CPU
（见[已知问题](../status/current/known-issues.md)），riscv64 为 4 CPU。启动时把对应客户端
目录传给 `GUI_MEDIA`：

```sh
tools/a20 check minecraft-aarch64
tools/a20 run minecraft-aarch64 -- GUI_MEDIA=build/minecraft/1.21.11-aarch64
tools/a20 run minecraft-riscv64  -- GUI_MEDIA=build/minecraft/1.21.11-riscv64
```

## 本轮实现

这些内核修复通过通用接口提供，未按 Minecraft 进程名或 ISA 名特判：

- CPU 信息报告实际在线 CPU 数，架构身份由 `kernel/arch` hook 提供。
- 普通路径与 openat2 共用挂载感知的符号链接遍历；GPU sysfs 暴露真实 PCI/MMIO
  身份，动态属性走文件 read，Mesa 可正确发现 GPU。
- VIRTGPU_RESOURCE_CREATE 分配独立 GEM 对象，动态存储通过 96 个同时存活对象测试。
- PRIME/DMA-BUF 导出保留真实 VMO，mmap 与 SCM_RIGHTS 共享同一对象；关闭 exporter
  句柄后仍保持有效。修复原先 memfd 快照以及 DRM_CLOEXEC 常量错误。
- 3D 上传/下载使用正确 wire 布局，scanout 使用宿主 3D 资源；提交等待真实 host
  GPU fence。sync_file 支持已完成 fence 的 poll、FILE_INFO、MERGE 与依赖检查。
- 大命令与 backing attachment 使用完成中断唤醒，保留启动阶段的有限轮询。
- 修复 SIOCGIFCONF 的 NULL 尺寸查询与输出缓冲区语义，使 Java 正常枚举网卡。

详见 [GPU 共享与同步](../graphics/virtgpu-sharing-and-sync.md)、
[GPU 发现](../graphics/drm-sysfs-discovery.md) 和
[网卡枚举](../kernel/linux-abi/interface-enumeration.md)。
会话根据 GPU 协商得到的 3D_FEATURES 自动选择 GLES 或 pixman，可通过
`A20_RENDERER` 覆盖，不用架构判断。

## 验证方法

`tools/tests/xfce-minecraft-smoke.sh` 是客体 QA 入口，只注入测试镜像副本的
labwc autostart，不进入生产自启动。默认使用独立的 `/root/.minecraft-qa`。
需要实际操作客户端，脚本不能把进程存在或菜单截图当作游玩通过。

1. 启动可写测试镜像，进入 demo 世界；用 F3 核对 virgl、Java、CPU 和坐标。
2. 移动玩家、打开背包，随后 Save and Quit to Title，再 Quit Game。
3. 等待脚本确认退出码 0、`level.dat` gzip 校验、SHA256 和 sync 完成。
4. 正常关机后离线读取存档，核对哈希；冷启动同一磁盘，实际重新进入世界并
   核对玩家位置。不要在世界重载过程中杀死 Java 或终止 QEMU。

原生 GPU 测试在 x86_64 与 RISC-V 四核抢占 guest 上通过：
`drm_prime_test`、`gpu3d_test`；2D fallback 也通过。对应门禁为
`make smoke-drm-prime`。ARM32 和 AArch64 的通用 GPU/文件操作改动已交叉构建。
这些测试本身不等于其他架构已运行 Minecraft 桌面；aarch64 与 riscv64 的
桌面启动验收见「多架构客户端」一节与文末验收记录。

## 当前边界

- 854×480、4 区块视距实测可以移动和操作背包；观察到约 8–18 FPS，菜单约
  20 FPS。这是功能验证中的现场读数，仍需性能优化，未达到稳定 30/60 FPS。
- OpenAL 使用 musl 系统库，sound engine 能启动；当前配置的设备是 No Output，
  尚未验收有声播放。原始 glibc C++ OpenAL 的异常路径不再进入 JVM。
- LWJGL C native 仍经 gcompat；启动器选择系统 allocator。没有宣称完整 glibc
  ABI 兼容，也没有为 jemalloc 注入补丁。
- Wayland 不支持绝对设置鼠标位置的 GLFW 提示仍可见；键盘移动与背包已经实测，
  鼠标转向和跳跃也已实测；全屏及真实硬件输入尚未系统验收。
- 默认 token 为 0，服务端会返回真实的 401；不把它误报为 DNS/TLS 故障。
  当前没有集成 Microsoft 账号登录，不宣称 Realms 或多人服务器可用。
- GEM 句柄命名空间仍是设备级，尚未实现 per-open 隔离；fence 当前同步完成，
  不是异步 GPU scheduler；DMA-BUF CPU 同步目前覆盖线性 level-0 纹理。

[组件状态](minecraft-components.md) 记录 JVM、native 和音频的具体范围。
[Firefox 验证](../status/history/xfce-x86_64-validation.md) 单独记录 Bing 联网结果。


## 2026-10-12 最终镜像记录

生产 world 为 `build/images/xfce-x86_64.img`，SHA-256：
`98bda983f0d05eec3f37211c938a8058230ba11edbcf1a00cb9128fd14ad1d43`。
只在其测试副本加入 QA 自启动；没有额外替换游戏 native 或渲染器配置。

- `build/browser-followup/minecraft-production-2.log`：自动选择 GLES、默认 Java
  ergonomics、新建 demo 世界，实际移动、背包、三个维度保存、GUI Quit Game
  退出码 0，gzip 与 sync PASS。
- 同一可写磁盘正常关机后，离线存档 SHA-256 与客体完全一致：
  `9301c340ffb1d86ba1bc29933801bf559d606725a486cfe7ac32f8aafc4e7b25`。
- `minecraft-production-3.log`：冷启动核对上述哈希、成功加载主存档，玩家位置
  `(-42.5, 71.0, 70.69999998807907)` 与此前保存一致。未走备份恢复界面。
  继续实测鼠标转向、移动、跳跃和背包，随后再次保存退出，退出码 0。
  第二次关机后的离线存档哈希仍与客体一致：
  `50b93f249fbec9b62d8a7d5a199c53b7f90b44f96afe090166405971d659695a`。
- 重载使用包含网卡枚举修复的内核 `desktop-final-kernel.elf`，SHA-256：
  `7bac9f1830ebba3f03ef183b02d7b905af3a8abd170975913f47b1642a3d9437`。
  实际游戏不再出现 `ioctl(SIOCGIFCONF) failed` 或 UnknownHostException。
- 同目录 `minecraft-production-{f3-before,inventory,reloaded-f3,playable}.png`
  保存 GPU/CPU、玩家状态和游玩画面。`playable` 显示转向到 15° 后玩家已经移动到
  `(-42.300, 69.123, 62.300)`，现场为 15 FPS。

早期调试曾在世界重载过程中终止 Java，得到不完整的 `level.dat`；那次只能
证明备份恢复可用，不计入上述冷启动验收。正常 GUI 退出、sync、关机后的
主存档已经按上述流程验证。
