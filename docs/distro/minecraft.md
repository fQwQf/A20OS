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
  接口 ioctl**，标准 DHCP/`ifconfig`/`ip` 都带不起接口。
- **已修（`8a66c9a4` + `29357cbc`）**：内核侧补齐了 SIOCGIF* 接口 ioctl 与 **AF_PACKET 裸 L2 socket**
  （`SOCK_DGRAM`/`SOCK_RAW`、`bind(sockaddr_ll)`、收发以太网帧；RX 经 lwIP 锁下的延迟环 + poll 下半部投递）。
  实测客体里 busybox `udhcpc` 走完整条 DHCP：

  ```
  udhcpc: broadcasting discover
  udhcpc: broadcasting select for 10.0.2.15, server 10.0.2.2
  udhcpc: lease of 10.0.2.15 obtained from 10.0.2.2, lease time 86400
  ```

  即 socket/bind/发/收全部打通。**仍未做**（udhcpc 不需要，但 `ip` 等工具要）：路由 netlink
  （`RTM_GETLINK`/`RTM_NEWADDR`）与 `net0` 在 `/proc/net/dev` 里的注册；`/usr/share/udhcpc/default.script`
  在套用租约时报 `arithmetic syntax error`（发行版脚本问题，与 socket 层无关）。

- **当前的阻塞点：游戏卡在 `glfwCreateWindow()` 里不返回（实测线程转储，不是猜测）**。网络修好之后，
  游戏能走完 Datafixer（287 项，1.6s）→ `Environment[...PROD]` → `Setting user: Player` →
  `Backend library: LWJGL version 3.3.3+5`，然后停在**创建窗口**这一步：MC 的深灰加载窗口已经出现在
  屏幕上，但渲染线程此后不再前进（观察 9 分钟无任何新日志）。期间唯一的活动是 authlib 每 5 分钟重试
  一次（`UnknownHostException: sessionserver.mojang.com` / `api.minecraftservices.com`——DNS 仍不通，
  但单机不需要它，不致命）。

  用 `kill -QUIT` 让 HotSpot 打线程转储（**必须把 launcher 的 stdout 重定向到 `/dev/console`** 才能从
  串口日志拿到它），渲染线程在 200 秒时的位置精确是：

  ```
  "Render thread" #1 ... cpu=-0.00ms elapsed=199.82s  java.lang.Thread.State: RUNNABLE
      at org.lwjgl.system.JNI.invokePPPP(Native Method)
      at org.lwjgl.glfw.GLFW.nglfwCreateWindow(GLFW.java:2058)
      at org.lwjgl.glfw.GLFW.glfwCreateWindow(GLFW.java:2229)
      at fyk.<init>(SourceFile:107)
      at hps.a(SourceFile:20)
      at gfj.<init>(SourceFile:504)
      at net.minecraft.client.main.Main.main(SourceFile:234)
  ```

  要点：**该线程 CPU 消耗约 0**、其余 12 个线程全部 WAITING/空闲、内核侧零 `[ERR]`/fault。
  即不是崩溃、不是 JVM 内部死锁，而是**阻塞在 native 的 X11 窗口创建里**：`XSync` 那类同步往返
  等不到 Xwayland 应答，或者 Xwayland 自己被卡住。它与
  `Xwayland glamor: GBM Wayland interfaces not available` + `falling back to sw`、以及紧随其后的
  一堆 xkbcomp warnings 出现在同一位置——XKB keymap 是一大块数据，正好走 Xwayland 那条 AF_UNIX 连接，
  所以「AF_UNIX 大消息/背压路径丢了唤醒」是很自然的怀疑对象。

  用户侧看到的现象（窗口一闪而过然后退出）是同一处卡死的另一种表现：X 窗口建出来、随即被拆掉，
  客户端永远等不到返回。

  **判别实验（尚未做）**：① 同一内核用 `-m 2G` 跑同一段注入——`-m 2G` 时新的 >4GiB RAM 映射完全不生效
  （见 `67b47c16`），若照样卡住就排除内存改动；② 卡住后在同一会话里跑 `xdpyinfo`/`xeyes`：新的 X 客户端
  也卡 ⇒ Xwayland 自己死了；只有 GLFW 卡 ⇒ 是那一条连接/那一次往返。

  另外记一笔：启动器当前继承到 `JAVA_TOOL_OPTIONS: -Xms256m -Xmx512m`（镜像里 `/usr/bin/java`
  包装脚本给的默认堆）。内存现在能到 4G 了，这个 512MB 上限迟早要抬。

  **两个控制实验都做完了，都排除了一批嫌疑**：

  - **`-m 2G` 对照**：`[RAM] usable` 只有 `0x100000..0x7ffd9000 (2046 MiB)`（此时 >4GiB 映射
    完全不生效），OOM 计数 0，渲染线程**卡在同一行**：`nglfwCreateWindow` RUNNABLE、
    `cpu=-0.00ms elapsed=199.77s`。⇒ **`67b47c16` 的内存改动与此无关**，这是既有 bug。
  - **延后启动对照**：把游戏从「会话起来后 30s」推迟到 **150s**（远晚于 swaybg 铺背景面的
    25–45s 窗口）再启动，仍然**卡在同一行**（`elapsed=149.75s`）。⇒ 也**不是** swaybg 与
    建窗撞车。

  即这是一个**确定性**的卡死，与内存大小、启动时机都无关，症状是：客户端在 native
  `glfwCreateWindow` 里等 Xwayland 应答而 ~0 CPU（纯阻塞），内核侧零 `[ERR]`。
  卡住的那一刻正好是 `xkbcomp` 输出 keymap 的时候 —— **XKB keymap 是一大块数据**，
  因此优先查 AF_UNIX STREAM 的大消息/唤醒路径：

  - `kernel/net/socket_unix.c:unix_ch_send()` 在 `A20_ERR_WOULD_BLOCK` 时**无条件返回
    `-EAGAIN`**（`return sent ? (int)sent : -EAGAIN;`），对**阻塞** socket 而言这不符合
    Linux 语义——阻塞写应当等待，而不是给上层一个短写/EAGAIN。
  - `net_unix_socket_sendto_impl()` 的**旧队列**路径（SCM_RIGHTS 那条）只唤醒**一个**
    等待者（`wait_queue_collect_one(&dst->read_waitq, 0, PROC_WAKE_EVENT, &wake_q)`），
    存在「读者判空 → 写者入队并唤醒（此时无等待者）→ 读者才 park」的丢唤醒窗口；一旦发生，
    双方都停住，与实测现象一致。（channel 路径用的是 `wait_queue_wake_all()`，那条没有这个问题。）

  **阻塞点已定位到具体 syscall（实测线程级 syscall 追踪）**：内核自带一个 bootarg 门控的追踪器
  （`kernel/syscall/trace.c`；`nm kernel.elf` 确认 `syscall_trace_enter/exit/slow_scanner` 都在，
  它会把 in-flight 的 syscall 作为 `[TRACE-SLOW]` 报出来）。它在 x86_64 上无法从宿主开启（原因见下），
  于是临时把它的 prefix 默认成 `java` 跑了一次（**该临时改动已还原，未提交**）。追踪把渲染线程
  卡住前最后的 syscall 序列钉死为：

  ```
  [TRACE] 159(Render thread) recvfrom(78 cf7e20c0 1faa0)    # 请求 129696 字节
  [TRACE] 159(Render thread) recvfrom = 40640               # 只拿到 40640（部分读）
  [TRACE] 159(Render thread) recvfrom(78 cf7ebf80 15be0)    # 再要剩下的 89056
  [TRACE] 159(Render thread) recvfrom = -11                 # EAGAIN
  [TRACE] 159(Render thread) poll(614ee360 1 ffffffffffffffff)
  [TRACE-SLOW] pid=159(Render thread) stuck in poll for 1.6e11 ticks (args 614ee360 1 ffffffffffffffff)
  ```

  即：**客户端读到一条大 X 回包的“前半截”（40640 / 129696）之后，剩下的字节再也没到，于是它对一个
  fd 做无限 `poll` 等在那里** —— 这就是 `glfwCreateWindow` 不返回的直接原因。所以问题不在窗口创建
  本身，而在 Xwayland↔客户端这条 AF_UNIX 流上「大消息只送出一部分，其余没被送出/没有重发」。
  头号嫌疑仍是 `unix_ch_send()`：通道返回 `A20_ERR_WOULD_BLOCK` 时它**无条件**
  `return sent ? (int)sent : -EAGAIN;`，而对**阻塞** socket 来说正确语义是等待后把剩余字节继续送完，
  不该把短写/EAGAIN 抛给上层；一旦发生，大回包的后半截就永远到不了，客户端无限 poll 等它。
  （按这条线索改过一版并实测：**没能修好**——MC 仍停在 `Backend library` 之后、没有 renderer 行，
  所以那版改动已还原，没有留在树里。）

- **换个进程追踪后又得到两个新事实**（把追踪 prefix 临时换成 `Xwayland` 跑的一轮，改动同样已还原）：

  1. **这一轮 MC 穿过了建窗**：日志里出现了 `[11:40:33] [Render thread/INFO]: Using optional
     rendering extensions: GL_ARB_buffer_storage, GL_KHR_debug, GL_ARB_vertex_attrib_binding,
     GL_ARB_direct_state_access, GL_EXT_texture_filter_anisotropic` —— 说明上面那个「无限 poll」
     **不是必然发生**，而是与调度/时序有关（追踪改变了时序就不触发）。这也解释了为什么单改
     `unix_ch_send()` 没用：真正的竞态在别处。同时 Xwayland 侧看：它的 `writev` 全是小包
     （`writev = 32`）、随后长期停在 `epoll_pwait`，**没有**看到「写一半就不写了」的证据。
  2. **这一轮最后内核 panic 了（新 bug，只观测到一次，可复现性未验证）**：

     ```
     [ERR] Kernel Address Error: code=1
     [ERR] Faulting PC (ERA): 0xffff8000002a0c99
     [ERR] Fault Address (BADV): 0x6fa0f1b8
     [ERR] Current Task: pid=0 name=idle
     [PANIC] backtrace:
       [0] kernel_trap_handler+0x372
       [1] ethernet_output+0x52a
     ```

     即**网络发送路径（lwIP `ethernet_output`）在 idle 任务上下文里踩了一个野指针**。
     时间点正好在 MC 尝试连 sessionserver/api.minecraftservices 之后（这轮那些请求报的是
     `SocketException: Broken pipe`，是 panic 的后果而不是原因）。怀疑与 AF_PACKET 那套
     （`29357cbc` 的 RX tap / poll 下半部）或延迟 TX 有关；修它之前先要能稳定复现并确认是不是
     同一处被踩坏。

- **已排除的假设（读码确认，别再追）**：通道「发送方等空间」的 park/wake 配对是**对的** ——
  发送方在 `peer->waiters` 上以 `A20_CH_WAIT_SEND` 挂起（`a20_channel.c:312`，`peer` 是接收端
  的 endpoint），接收方 `a20_channel_recv_finish()` 在**同一把** `peer->lock` 下让出空间并唤醒
  `ep->waiters`（`a20_channel.c:618`），两者是同一条队列且对锁原子，不存在丢唤醒。
  同理 `unix_ch_recv()` 对半消费消息的暂存（`s->ch_buf`/`s->ch_len` + `memmove`）逻辑也是对的。
  ⇒ 客户端那次「只拿到 40640 就 EAGAIN」确实是**当时通道里就这么多**，问题在更上游：
  要么 Xwayland 没把剩下的写出去，要么写了但在到达客户端前被丢掉。
  **下一步（唯一还没做的观测）**：在**真的卡住**的那一轮里同时追踪 Xwayland 与客户端，
  看 `fd 78` 上那条大回包的剩余部分有没有出现在 Xwayland 的 write/writev 里；
  要留意多个线程并发 `recv` 同一个 socket 时 `unix_ch_recv()` 的暂存缓冲没有加锁这一点
  （它会丢字节，且丢字节在流上是不可检测的——症状正好是「剩余部分永不到、客户端无限 poll」）。

  **定位手段本身也撞墙了（已实测）**：内核自带一个 bootarg 门控的 syscall 追踪器
  （`kernel/syscall/trace.c`，`trace=<comm-prefix>`，并会把 in-flight 的 syscall 作为
  "slow" 报出来——"最后一个只有 enter 没有 return 的就是阻塞点"，正是查这种卡死要的东西；
  `nm kernel.elf` 确认 `syscall_trace_enter/exit/slow_scanner` 都编进去了）。**但它打不开**：

  - `-append "trace=java"` 无效：x86_64 的 bootargs 走 fw_cfg 的 `FW_CFG_CMDLINE_SIZE/DATA`
    （`kernel/arch/x86_64/platform/firmware.c` 用 0x0014/0x0015），而 **QEMU 只在 Linux boot
    protocol 下才填这两个 key**；本内核是 multiboot `-kernel kernel.elf`，于是客体内实测
    `[FW_CFG] cmdline_size=0` / `cmdline=''`。
  - `-fw_cfg name=etc/cmdline,string=...` 也没用（那注册的是另一个文件选择子，不是 0x0014/0x0015）。

  ⇒ **x86_64 上 `-append` bootargs 实际上是坏的**（`kernel/platform/qemu-virt-x86_64/board.c`
  的注释声称走 `-append`，与实测不符；平时被静态网络兜底掩盖了）。另外实测 QEMU 在 multiboot 这条
  路径上也没有把 `-append` 传进内核（multiboot info 的 cmdline 同样为空），所以追踪器只能用上面
  那次临时默认才跑得起来 —— 修好 bootargs 这条链本身（改用 multiboot cmdline 作来源）就值得单独做。

  **要接通观察手段**，二选一：① 让 x86_64 去读 **multiboot** 的 cmdline（QEMU 的 multiboot info
  里有，顺带修掉上面那个 bootargs bug）；② 临时把 `trace.c` 的 prefix 默认成 `java`。
  接通后再跑 `trace=java`，读那条 slow/in-flight 记录即可定位阻塞 syscall。

  **另外提醒**：`/proc/<pid>/wchan` 与 `/proc/<pid>/stack` 目前是**残桩**，不能用——
  wchan 对**每个**任务（连 `dbus-daemon` 都算）都返回同一个 `do_wait`，stack 打印
  `[<0000000000000000>] 0`，且没有 `/proc/<pid>/task/`。




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
