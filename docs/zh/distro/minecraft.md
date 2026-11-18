# Minecraft Java 客户端

A20OS 的 XFCE 镜像可在 x86_64、aarch64 与 riscv64 上运行 Minecraft Java 1.21.11，使用 OpenJDK 21、原生 Wayland 窗口和 virgl OpenGL。默认桌面入口启动官方 demo，便于在没有登录会话的测试镜像中验证世界生成和交互。账号登录、Realms 和联机尚未验收。

## 构建和启动

宿主机需要 QEMU virgl 支持、可用的 OpenGL 显示或 EGL render node。测试使用 q35/KVM、4 CPU、4 GiB 和 AMD Radeon 780M；普通 2D virtio GPU 仍可运行 XFCE 与 Firefox，但不建议用于 Minecraft。

```sh
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11
tools/a20 check minecraft-x86_64
tools/a20 run minecraft-x86_64 -- GUI_MEDIA=build/minecraft/1.21.11
```

`instances/minecraft-x86_64.toml` 使用 4 GiB 内存、4 GiB XFCE world 和 virgl GPU。下载脚本读取 Mojang 版本清单并按 SHA1 校验客户端、library 和资源。本次完整数据为 107 个 library、9 个 native、4591 个 asset 对象，资源索引 29。镜像内文件位于 `/usr/share/a20-media/1.21.11`；客户端数据不会提交进 Git。

桌面镜像自动打包 `build/minecraft/*` 中原生库 ELF 架构与目标架构一致的客户端。客体 `minecraft` 命令再次根据 `uname -m` 检查原生库；同时存在多个架构的资源时，不会因目录排序而选中 RISC-V。显式 `MC_HOME` 也必须匹配，游戏版本从 `version.json` 读取，目录名中的架构后缀不会进入版本号。

在已经启动的桌面中，点击 Minecraft 菜单入口或运行：

```sh
minecraft --demo
GAMEDIR="$HOME/.minecraft-test" minecraft --demo --width 854 --height 480
MC_MIN_HEAP=256m MC_MAX_HEAP=1536m minecraft --demo
```

启动器使用桌面已有的 `WAYLAND_DISPLAY` 与 `XDG_RUNTIME_DIR`，清除本进程的`DISPLAY` 以选择 GLFW Wayland 后端。它为新游戏目录复制版本匹配的初始配置： Custom、4 区块视距、5 区块模拟距离、关闭云和 mipmap。已有玩家配置不会覆盖。 Java wrapper 只定位真实 JVM，不再全局注入堆上限或关闭隐式空指针检查； Minecraft 的堆设置局限于自身启动器。

## 多架构客户端（aarch64 / riscv64）

Minecraft Java 1.21.11 固定使用 LWJGL 3.3.3；Mojang 只发布 x86_64 的 Linux native，因此其他架构按下表选择 LWJGL 来源。loongarch64 目前在任何发行渠道都没有 LWJGL native，下载脚本会直接报错退出，不会产出不完整的客户端目录。

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

Maven 路径会跳过版本清单中的全部 org/lwjgl 条目（普通 jar 与 native jar 都由 Maven 版本取代），下载后校验 SHA1，解包 native 时校验 ELF magic 与 e_machine（aarch64=183，riscv64=243），避免混入宿主或错误架构的 .so。 client.jar、其余 library 与 4591 个 asset 对象三架构一致；aarch64 / riscv64 的 classpath 为 67 个 jar（51 个原版 + 16 个 LWJGL），解包 8 个 LWJGL native .so。

对应实例 `instances/minecraft-{aarch64,riscv64}.toml` 与 x86_64 相同： 4 GiB 内存、4 GiB XFCE world、virgl GPU；aarch64 因 SMP 竞态固定单 CPU （见桌面当前状态），riscv64 为 4 CPU。启动时把对应客户端目录传给 `GUI_MEDIA`：

```sh
tools/a20 check minecraft-aarch64
tools/a20 run minecraft-aarch64 -- GUI_MEDIA=build/minecraft/1.21.11-aarch64
tools/a20 run minecraft-riscv64  -- GUI_MEDIA=build/minecraft/1.21.11-riscv64
```

## 验证方法

`tools/tests/xfce-minecraft-smoke.sh` 是客体 QA 入口，只注入测试镜像副本的 labwc autostart，不进入生产自启动。默认使用独立的 `/root/.minecraft-qa`。需要实际操作客户端，脚本不能把进程存在或菜单截图当作游玩通过。

1. 启动可写测试镜像，进入 demo 世界；用 F3 核对 virgl、Java、CPU 和坐标。
2. 移动玩家、打开背包，随后 Save and Quit to Title，再 Quit Game。
3. 等待脚本确认退出码 0、`level.dat` gzip 校验、SHA256 和 sync 完成。
4. 正常关机后离线读取存档，核对哈希；冷启动同一磁盘，实际重新进入世界并核对玩家位置。不要在世界重载过程中杀死 Java 或终止 QEMU。

原生 GPU 路径的回归门禁是 `make smoke-drm-prime`，包含 `drm_prime_test` 与 `gpu3d_test`； ARM32 与 AArch64 的通用 GPU/文件操作改动只做交叉构建。这些用例不等于其他架构已运行 Minecraft 桌面。
