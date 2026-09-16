# 在 A20OS 镜像里装 Minecraft Java 版

Minecraft Java **不是一个 jar**。一个"版本"= 客户端 jar + 一组 library（LWJGL、netty 等）
+ 一套 asset 资源库，官方启动器就是按版本清单把这三样配齐。所以这里给的是
「取件 → 装进镜像 → 启动」三步，全部走 Mojang 自己的公开端点（`piston-meta.mojang.com`），
每个文件都按清单里公布的 SHA1 校验。

**授权**：玩游戏需要一个 Minecraft 账号（用官方启动器登录）。这里的脚本只帮**已授权的用户**
把文件备好，下载的全部是 Mojang 本来就在公网提供的内容，不绕过任何授权。

## 0. 版本与 JVM 的对应关系（先确认，否则必然跑不起来）

| 版本线 | 需要的 Java |
|---|---|
| `1.21.6` – `1.21.11` | **Java 21** ← `xfce` 镜像现在装的就是 `openjdk21-jre` |
| `26.x`（当前是 `26.3`） | **Java 25**（Alpine 有 `openjdk25`，需自行加进 world） |

所以默认选 `1.21.11`：它是**不需要动镜像 JVM 的最新版本**。

## 1. 取件（在宿主机上）

```bash
tools/fetch-minecraft.sh                        # 默认 1.21.11
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11
WITH_ASSETS=0 tools/fetch-minecraft.sh          # 只要 jar+library，跳过 461MB 的 assets
```

产物布局（就是官方启动器用的布局）：

```
build/minecraft/1.21.11/
    client.jar            # 客户端 jar（39MB 级）
    version.json          # 该版本清单
    libraries/<maven 路径>.jar   # 107 个 library
    classpath.txt         # 冒号分隔的 classpath
    natives/*.so          # 从 *-natives-linux.jar 里解出的 LWJGL native
    assets/indexes/<id>.json     # 资源索引
    assets/objects/<xx>/<hash>   # 资源本体（约 460MB、数千个文件）
```

`natives/` 是**在宿主机解好**的——客体镜像里没有 `unzip`。

## 2. 装进镜像

镜像自带的 `GUI_MEDIA` 机制就是干这个的：它生成一个**构建期 overlay**
（`build/overlay-media/<world>-<arch>/usr/share/a20-media/`，由 `mkrootfs.py --overlay` 合入），
并把 `/root/Desktop/a20-media` 软链过去。用构建期 overlay 而不是往现成镜像里写，是因为
**在客体里新建目录常常看不到**（debugfs 写入新目录的已知问题）。

```bash
make ARCH=x86_64 image-world PKG_WORLD=xfce PKG_SIZE_MB=4096 \
     GUI_MEDIA=build/minecraft/1.21.11
```

两个必须注意的点：

- **`PKG_SIZE_MB=4096` 不能省。** `make` 的默认值是 `512`，加进 ~600MB 的游戏会撑爆。
  （用 `tools/a20 run xfce-x86_64` 时由 `instances/xfce-x86_64.toml` 的 `world_size_mb = 4096` 兜住。）
- 游戏会落在镜像内的 `/usr/share/a20-media/1.21.11/`，桌面上的 `a20-media` 软链可以直接进。

## 3. 启动

镜像里带了启动器 `/usr/local/bin/minecraft`（源码在
`packages/overlay/xfce/usr/local/bin/minecraft`），它负责拼 classpath、指向 `natives/`、
带上 `--assetsDir/--assetIndex` 调 `net.minecraft.client.main.Main`。

在**桌面会话内**运行（GLFW 的 Wayland 后端需要会话环境）：

```bash
XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0 minecraft
USERNAME=Steve minecraft          # 单人游戏的显示名
MC_HOME=/usr/share/a20-media/1.21.11 minecraft
```

## 已验证 / 未验证

**已验证（有实测证据）**：

- **取件脚本产出的布局就是启动器要的布局**：107 个 library、106 条 classpath、9 个 native 已解出、
  4591 个 asset 对象，全部按清单 SHA1 校验，总计 553MB。
- **装载方法可用且不损坏文件**：用 `GUI_MEDIA` 重建镜像后，从镜像里 dump 出 `client.jar` 与宿主副本
  **逐字节一致**（sha1 `ba2df812c2d12e02`、31152600 字节、zip 魔数 `PK\x03\x04`、28163 个条目、
  `net/minecraft/client/main/Main.class` 存在）。启动器 `/usr/local/bin/minecraft` 与
  `/root/Desktop/a20-media` 软链都在镜像里。
- LWJGL 的 native 能加载（`gcompat` + `libdl.so.2` 软链，见 `3d-graphics.md` §9.14/§9.15）。
- **启动器本身工作正常**：客体里 `minecraft` 能跑起来，正确解析出版本 `1.21.11` 与 asset index `29`，
  并拉起 `java`。

**未跑通，以及为什么（实测，不是猜测）**：

- **游戏卡在 `ClassNotFoundException: net.minecraft.client.main.Main`**，但**这不是 classpath 拼错**——
  该类确实在 jar 里（28163 条目中的一个）。隔离测试（只用 `client.jar` 作 classpath）暴露了真正的根因：
  JVM 读取 jar 时**反复触发对齐异常**：

  ```
  ADE/ALE: pid=165 sepc=0x418739a0 stval=0xc code=1
  [ERR] a3=0x8080808080808080        # SSE 向量化写的典型模式
  ```

  x86_64 上普通非对齐访问本不该陷入（只有 `movdqa` 这类要求对齐的指令会），而日志显示它**反复发生**，
  说明**内核对对齐异常（#AC）的处理有问题**——JVM 因此读不出 jar 内容，才报类找不到。
  这是**内核 bug**，不是 Minecraft 或镜像配置的问题；修内核之前游戏不可能起来。

**另一个独立的内核 bug**：把内存从 2G 加到 3G 启动时，内核在早期启动阶段 **panic**：

```
[BUS] pci 00:02.0 id=1af4:1050 ...
[ERR]   [0] pc=pci_virtio_write32+0x2db
========== KERNEL PANIC ========== Unhandled kernel page fault
[PANIC] task: <none/early boot>
```

即**我的内存修复（`884ef373`）只覆盖了 2G，3G 时在 PCI virtio 探测期间页错误**。
Minecraft 需要大于 2G 的堆，所以这个 bug 也是前置障碍。两者都需要新的内核修复。

**桌面启动器的参数是按官方启动器拼的**，在类加载问题修好之前无法判断是否需要微调。

## 已知限制

- **GL 走 llvmpipe（软件渲染）**，因为 GBM 路径在本内核下不通（原因见 `3d-graphics.md` §9.13），
  合成器是 `WLR_RENDERER=pixman`。游戏能起也大概率很慢。
- `assets` 有数千个小文件，`fetch-minecraft.sh` 是逐个下载的，会花十几分钟。
- 镜像尺寸：`xfce` 镜像本身约 1.2GiB，加游戏约 +600MB，`PKG_SIZE_MB=4096` 够用但别再往上堆大件。
