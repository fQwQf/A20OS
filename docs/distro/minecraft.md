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

- **游戏曾卡在 `ClassNotFoundException: net.minecraft.client.main.Main`**，但**这不是 classpath 拼错**——
  该类确实在 jar 里（28163 条目中的一个）。隔离测试（只用 `client.jar` 作 classpath）暴露了真正的根因：
  JVM 读取 jar 时 JVM 自己的 SIGSEGV 处理器**反复触发 #GP**：

  ```
  ADE/ALE: pid=165 sepc=0x418739a0 stval=0xc code=1
  [ERR]   insn@sepc=0x2444290f          # 0f 29 44 24 .. = movaps [rsp+X], xmm0
  ```

  `code=1` 是 `CAUSE_INSN_FAULT`（**x86_64 上只由 #GP 产生**，不是 #AC 对齐异常），
  而 `stval` 对 #GP 是**过期的 CR2**（#GP 不写 CR2），所以 `0xc` 是误导。
  真因是**信号处理器入口栈对齐错了**：内核把 handler 入口 `rsp` 设成 16 对齐
  （≡0 mod 16），而 x86_64 SysV ABI 要求函数入口 `rsp ≡ 8 (mod 16)`。
  handler 的编译器序言据此对齐，于是它的 `movaps [rsp+X], xmm0` 在错位 8 字节的栈上
  触发 #GP；该 #GP 又落在 handler 里 → 内核再次投递 SIGSEGV → 再压一帧 → 无限递归。

  **已修（`68abf68f`）**：帧基保持 16 对齐（内嵌的 fxsave64 区域必须 16 对齐），
  但 handler 在帧基下方 8 字节处进入，sigreturn trampoline 地址就放在那里给 handler 的 `ret`。
  修复后 `ADE/ALE` 归零，JVM 的 SIGSEGV 处理器**能正常运行**（能打印崩溃报告），
  桌面无回归。

- **根因是 HotSpot 的隐式空指针检查（implicit null check）**，不是内存被写坏。JVM 读 jar 清单
  算 SHA-384，`SHA5.implCompress0` 里一条数组访问依赖「空数组会 fault、SIGSEGV 处理器再抛 NPE」
  这套机制；A20OS 上这条路径没被识别成空检查，于是 JVM 把它当致命崩溃。最小复现器
  （宿主 javac 编 `.class` 注入客体跑）分别试了 `o.hashCode()`（字段/虚调用）和 `arr[0]`（数组访问）：
  前者正常抛 NPE，后者直接把 JVM 打崩。用
  `-XX:+UnlockDiagnosticVMOptions -XX:-ImplicitNullChecks` 关掉隐式检查后，
  `java -cp client.jar Main` **不再崩**，只在 `joptsimple/OptionSpec`（本就不在 client.jar 里）上报
  `NoClassDefFoundError` —— 即 jar 读路径本身已经通了。**这是 JVM 侧规避，不是根治**。

- **ucontext 布局（已修 `2c1d0dbe`）**：`hs_err` 曾报 `bad uc->uc_mcontext.fpregs: 0x0`，
  "Problematic frame" 打印本身也一直 fault、`RIP=0`。A20OS 的 `uc_sigmask` 排在 `uc_mcontext`
  **之前**、mcontext 用私有寄存器序、FPU 直接内嵌；Linux/musl 则是 `uc_mcontext`（`gregs[23]`
  按 REG_R8..REG_CR2 序）在前、`fpregs` 为指向 `__fpregs_mem[64]` 的**指针**。已按 musl 的
  `ucontext_t`/`mcontext_t` 对齐，`_Static_assert` 钉死偏移与尺寸。修完 `fpregs` 报错消失、
  崩溃报告能正确给出 "Problematic frame"（`J ... SHA5.implCompress0`）。

- **同步 SIGSEGV 的 siginfo（已修 `4cf43e37`）**：原先 `si_code`= `SI_KERNEL`、`si_addr` 恒 0；
  Linux 用 `SEGV_MAPERR` + 故障地址。已按 Linux 填（HotSpot 会据此判空检查）。

- **启动器两个 classpath bug + 两个规避（已修 `f3469887`）**：`classpath.txt` 是**单行冒号分隔**
  （0 换行、106 个冒号），启动器却用贪婪 `sed 's|^.*/libraries/|…|'` 改前缀——`^.*` 吃到全行
  最后一个 `/libraries/`，把整个 classpath 塌成一条；它又把 `client.jar` 直接粘在（无结尾冒号的）
  CP 之后，于是 client.jar 根本没进 classpath → 就是一开始那个 `ClassNotFoundException`。
  已改成逐条（`[^:]*`）重写并补结尾冒号。启动器另带两个 A20OS 规避：
  `-XX:+UnlockDiagnosticVMOptions -XX:-ImplicitNullChecks` 与 `-Dos.name=Linux`
  （LWJGL/Minecraft 拒绝未知平台名 "A20OS"）。

- **当前能走到哪**：修完上面这些，启动器能跑完整套真实启动流程——Datafixer（287 项优化）、
  Environment/sessionHost、`Setting user: Player`、进入 Render thread、
  **`Backend library: LWJGL version 3.3.3+5` 成功加载**，并且**开窗成功**（XIO 错误没有了）。
  这中间还修/绕了两处：
  - LWJGL 自带的 `libjemalloc.so` 会 fault（`C [libjemalloc.so+0x10055] _init+0x8055`，pc=0x81a6）；
    用 `-Dorg.lwjgl.system.allocator=system` 换掉分配器即绕过。
  - 开窗时 GLFW 走 Xwayland 的 X11 路径，曾报
    `XIO: fatal IO error 90 (Message too large) on X server ":0"`：A20OS 的 AF_UNIX 旧队列路径把单条消息
    卡在 `NET_MAX_PAYLOAD`(64KiB)，更大的写直接 `EMSGSIZE`。**已修（`4e2aeb9f`）**：STREAM 写超过一条
    消息时按 64KiB 分片（读端仍是同一字节流）。
- **GL 链对 Minecraft 是通的（实测）**：开窗之后游戏继续初始化 OpenGL 渲染器并成功，日志打出
  `Using optional rendering extensions: GL_ARB_buffer_storage, GL_KHR_debug, GL_ARB_vertex_attrib_binding,
  GL_ARB_direct_state_access, GL_EXT_texture_filter_anisotropic`（GLX/Xwayland + llvmpipe），
  **没有崩溃**。即游戏已经渲染起来了，剩下的不是「起不来」而是这些收尾项。
- **下一个阻塞点是网络/DNS**：游戏走到网络/鉴权阶段（Yggdrasil 公钥、session profile 查询）时报
  `UnknownHostException: api.minecraftservices.com`。xfce overlay 原先既没有 `/etc/hosts` 也没有
  `/etc/resolv.conf`，已补上（`bff441f7`；resolv.conf 指 QEMU user-mode 的 `10.0.2.3`，与 pynode overlay 一致）。
- **光补 resolver 不够（实测）**：客体里 `net0` 只出现在 `/sys/class/net`，`/proc/net/dev` 是空的、也没有路由
  —— 接口根本没起来。`udhcpc -i net0` 卡在 `ioctl 0x8933`（SIOCGIFHWADDR）`Not a tty`；`ip addr` 也报
  `socket(AF_NETLINK,3,0): Protocol not supported`。即 A20OS 的网络栈（lwIP）**没有实现 SIOCGIF* 这套
  接口 ioctl**，标准 DHCP/`ifconfig`/`ip` 都带不起接口（这也是为什么 /proc/net/dev 里没有 net0）。
  真实实例是**用别的方式**配网的（`curl` 可通，网络不阻塞），最小测试启动没配。要让客体常规网络工具链可用，
  需要在内核侧补 SIOCGIF* ioctl 与 netdev 的 /proc/net/dev 注册 —— 属于网络子系统/发行版配置，
  不是 JVM/Minecraft 这条线的问题。



**另一个独立的内核 bug（已修）**：把内存从 2G 加到 3G 启动时，内核在早期启动阶段 **panic**：

```
[BUS] pci 00:02.0 id=1af4:1050 ...
[ERR]   [0] pc=pci_virtio_write32+0x2db
========== KERNEL PANIC ========== Unhandled kernel page fault
[PANIC] task: <none/early boot>
```

根因：3G 时 QEMU 把 virtio-gpu 的 64-bit BAR 放到 `0xc000000000`（>4 GiB），
而入口 `boot_pdpt_hh` 只直接映射了物理 0–4 GiB，`arch_pci_bar_to_resource` 直接加
`PAGE_OFFSET` 得到一个不可达地址，于是早期探测 virtio 时写它就页错误。
**已修（`568cf392`）**：`pci_assign_bars` 把任何地址 ≥ `PHYS_MAP_LIMIT`（4 GiB）
的非 I/O BAR 重新分配到覆盖窗口内。实测 `-m 3G` 无 panic、桌面起来。
（注意 RAM 仍被内存图裁剪在 2 GiB，是另一条已记录的边界。）

**桌面启动器的参数是按官方启动器拼的**，在类加载问题修好之前无法判断是否需要微调。

## 已知限制

- **GL 走 llvmpipe（软件渲染）**，因为 GBM 路径在本内核下不通（原因见 `3d-graphics.md` §9.13），
  合成器是 `WLR_RENDERER=pixman`。游戏能起也大概率很慢。
- `assets` 有数千个小文件，`fetch-minecraft.sh` 是逐个下载的，会花十几分钟。
- 镜像尺寸：`xfce` 镜像本身约 1.2GiB，加游戏约 +600MB，`PKG_SIZE_MB=4096` 够用但别再往上堆大件。
