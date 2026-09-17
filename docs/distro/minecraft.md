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
     `SocketException: Broken pipe`，是 panic 的后果而不是原因）。

     **已复现（第二次，签名完全一致）**：又一轮 MC 运行又踩到同一处，这次记录更完整：

     ```
     [ERR] Kernel Address Error: code=1
     [ERR] Faulting PC (ERA): 0xffff8000002103ee
     [ERR] Fault Address (BADV): 0x71bc3000
     [ERR] Current Task: pid=0 name=idle
     [ERR]   backtrace:
     [ERR]     [0] pc=memcpy+0x34e
     [PANIC] caller=kernel_trap_handler+0x372
       [1] ethernet_output+0x52a
     [PANIC] attempting firmware poweroff
     ```

     panic 点落在 **`memcpy`**（`memcpy+0x34e`），**BADV=0x71bc3000 是用户态量级的地址**
     （不是内核栈/内核堆地址），而 `ethernet_output` 里唯一的大块 memcpy 就是往 pbuf 的
     payload 上写以太网头 ⇒ **到达 TX 的 pbuf 本身已经坏了**（payload 是野值）。
     `[1]` 那一帧的调用者 `0x7d0a60eb1e46d090` 同样是垃圾地址，说明**栈也被踩过**。
     两次 BADV 都是 ~0x6f–0x71 开头的用户态量级值（上次 `0x6fa0f1b8`），更像**同一处
     pbuf/缓冲所有权错误**，而不是随机内存损坏。整机在此 panic 后 poweroff，
     所以那一轮 MC 什么都没跑到（日志里没有任何 MC 进展行）。

     **已排除（读码确认，别再追）**：
     - **AF_PACKET 的 TX 不是这条路**：`a20_lwip_packet_tx()`（`lwip_stack.c:508`）是**同步**
       直接 `st->ops->send()`，不建 pbuf、也不经过 `ethernet_output` ⇒ `29357cbc` 那套
       RX tap / poll 下半部与它无关。
     - **不是「把用户态缓冲塞进 pbuf」**：所有发送路径都**拷贝**——UDP 是
       `pbuf_alloc(PBUF_TRANSPORT, …, PBUF_RAM)` + `pbuf_take()`（`socket_inet.c:1009/1042`），
       TCP 是 `tcp_write(…, TCP_WRITE_FLAG_COPY)`（`socket_inet.c:1158`），RX 是 `PBUF_POOL` +
       `pbuf_take()`（`lwip_stack.c:331/337`）。**没有任何 `PBUF_REF`/`PBUF_ROM` 指向用户态内存**。
     - `ethernet_output` 的调用者只有 lwIP 自己的 IPv4/IPv6 发送路径
       （`etharp.c:487/770/897/1008/1161/1165`、`ethip6.c:101/120`），即**普通 IP 发包**，
       不是 raw socket 路径。

     ⇒ 下一步顺着「**谁把已经坏掉的 pbuf 交给 TX**」查：ARP 排队（`etharp_query` 会把 pbuf
     挂进 `arp_table` 延迟发送）、TCP 重传队列（已 `tcp_write` 但未 ACK 的段）、以及
     `netif_poll`/下半部在 **idle 上下文**跑 TX 的时机——panic 时正是 **idle 任务**在发 TX，
     说明是**异步排队过的包**在发送时踩的，重点是这些 pbuf 的所有权/生命周期（谁释放、谁还引用）。

     **复现频率 + 新线索（重负载相关）**：又跑了 5 轮 MC，其中 **3 轮**踩到它（签名完全一致），
     有一轮连 GL 都没跑到就把整机 poweroff 了。关键相关性：**三次出现都在「重负载 / 大量控制台
     输出」的轮次**——第一次是开了 syscall tracer（`trace=Xwayland`）那轮，后来的两次是带了
     「每 0.2s 扫一遍 `/proc/*/maps`」辅助脚本的轮次。⇒ 更像**时序/竞态相关**，不是某条固定输入
     路径踩的。结合「**idle 上下文**在发 TX + pbuf 已经坏」，下一步优先查 `sys_check_timeouts()`
     驱动的 **TCP 重传 / ARP 定时器**（它们在 idle 里跑，而 MC 连 sessionserver 时正好有未 ACK 的段）。

- **【决定性】abort 来自 Mesa 的 LLVM 路径，不是 JVM**：用
  `GALLIUM_DRIVER=softpipe LIBGL_ALWAYS_SOFTWARE=1` 跑 MC（Mesa 的无 LLVM 纯 C 驱动）：

  | | 默认（llvmpipe/LLVM） | `GALLIUM_DRIVER=softpipe` |
  |---|---|---|
  | `std::system_error` | 每次都有 | **0 次** |
  | `[ERR]` 行 | 0 | 0 |
  | 内核 panic | 视轮次 | 0 |
  | MC 存活 | **t+45s 已死** | **t+60s 仍在、t+120s 仍在** |

  ⇒ 抛 `std::system_error` 的 C++ 代码在 **Mesa 的 LLVM 路径**（`libgallium-25.2.7.so` /
  `libLLVM.so.21.1`，两者都在镜像里）里，**不是 JVM**；这也和「libstdc++ 是在 GL 初始化那一刻才被
  `dlopen` 进来」的观测吻合（LLVM/gallium 是 C++，会带进 libstdc++）。
  代价：softpipe 那一轮**卡在渲染器初始化**（LWJGL 之后 >2.5 分钟没有新输出），所以它只是
  **定位手段**，不是可用的 workaround。下一步：查 LLVM/gallium 在这里起的线程（`std::thread`
  构造失败会抛 `std::system_error`）为什么失败——即 `clone`/`pthread_create` 在 A20OS 上的行为。

- **【可复用】gdbstub 抓 throw 现场的配方（已打通，断点已能正确装上）**：
  1. QEMU 加 `-gdb tcp::1234`；
  2. 客体侧辅助脚本打印 `LIBSTDCXXBASE <hex>`：对每个 `/proc/<pid>/maps` 里
     `grep -m1 libstdc++`，`comm` 匹配 `java*`，取 `cut -d- -f1`；变了才打印（避免刷屏）；
  3. 宿主侧等它出现 → 算地址 → gdb `hbreak`。`libstdc++.so.6.0.34` 里的偏移：
     **`__cxa_throw = 0xc9490`**、**`_ZSt20__throw_system_errori = 0xb4fa9`**（就是抛我们这条异常的函数，
     它的参数 `$edi` 就是 errno）、`_ZSt9terminatev = 0xb056a`。
  **两个已踩过的坑**：① 辅助脚本打的 base **没有 `0x` 前缀**，宿主 `$((B + off))` 会把十六进制串
  当成变量名（未定义＝0）从而把断点装到 `0xb4fa9` 上 ⇒ 必须写 `$((0x$B + off))`；
  ② gdb 命令文件要用**引号 heredoc**（`<<'EOF'`）生成，否则 `printf "... $edi"` 里的 `$edi`
  会被 shell 展开/吃掉。
  **ASLR**：`kernel/mm/elf.c` 的 `ASLR_BITS 11` + `elf_aslr_bias()`（`bits << 16`），每次启动 base 都不同
  （实测 `cf7ae000` / `cf721000` / `cf7b3000` / `cfa80000`）⇒ 必须每轮动态取 base；想固定的话，
  临时把 `elf_aslr_bias()` 改成 `return 0;` 即可（诊断用，记得还原）。
  本轮断点已按正确地址（`0xcfb34fa9`）装上，但那一轮 MC 卡在 LWJGL 之后**没有抛异常**，
  所以**还没拿到 throwing caller 与 `$edi`** —— 下一轮只要它一抛，`bt` 和 errno 就自动落盘。

  **后续两轮把「等 base 再装断点」这条路走死了（重要，别再重复）**：
  ① 断点按正确地址装好、那一轮 MC 也确实抛了（`terminate ... std::system_error`），
  但 gdb **0 次命中**——因为 `libstdc++` 是**在 GL 初始化那一刻才被 `dlopen` 进来**的，
  从 base 打印出来到真正抛异常只有 **~1–2 秒**；等宿主 watcher 起 gdb（~0.5–1s）再
  `target remote`（连接时还会把客体暂停一下）就已经晚了。
  ② 想用「关掉 ASLR 让 base 固定」来消除这个竞态：把 `kernel/mm/elf.c` 的 `elf_aslr_bias()`
  临时改成 `return 0;` 重新编译内核后，**base 依然每次不同**（实测 `cf4f3000` → `cf9d4000`）
  ⇒ **`elf_aslr_bias()` 只影响 `elf_load()` 自己的 load_bias，管不到动态加载器 `dlopen` 一个 .so 时
  自己 `mmap` 出来的地址**（那走的是内核 mmap 分配器，不经过这个 bias）。这条路**作废**
  （临时改动**已还原**，源码回到随机 bias）。
  ⇒ 要抓这个 throw 必须换手段。**注意：走 gdb/gdbstub 这条路已经彻底堵死**——先写了最小 RSP
  客户端（连上后直接发 `Z1` 硬件断点），又做了「让 gdb 常驻在提示符、用 FIFO 在 base 出现的瞬间
  喂命令」的版本（把 gdb 的启动延迟从 ~1s 降到 ~0），两种做法撞到**同一堵墙**：
  **QEMU 的 gdbstub 目标直接回报 `No hardware breakpoint support in the target`** ✗✗。
  这正好解释了此前**每一次** `hbreak`（包括地址完全正确、MC 也确实抛了的那一轮）都是 **0 次命中**：
  **不是时序问题，是目标根本不支持硬件断点**。而软件断点（gdb `break`）要往**客体进程的地址空间**
  写 `int3`，gdb 挂在裸内核上做不到 ✗。⇒ **唯一剩下的办法是客体侧自测量**：

  - **(a) `LD_PRELOAD` 垫片**：覆写 `__cxa_throw` / `_ZSt20__throw_system_errori`，把返回地址（调用方）
    与 `%edi`（errno）直接写到 stderr。关键是让它**不依赖任何 libc**（只用原始 `syscall` 指令），
    这样宿主 `gcc -shared -fPIC -nostdlib` 就能编出可用的 .so，绕开「没有 musl 交叉工具链」这个障碍。
  - **(b) 客体侧探针命令**：仓库已有先例和现成的构建路径（`user/cmds/core/fdprobe.c`、`mmprobe.c`、
    `race_probe.c`，产物在 `user/build/x86_64/`），照它加一个专门触发/观测这个 throw 的命令即可。

  **(a) 已经做出来了，而且确实能装进客体**：一个 freestanding 的 `LD_PRELOAD` 垫片，只导出
  `_ZSt20__throw_system_errori` 与 `__cxa_throw`，两者都先把一行
  `THROW_SYSERR a=0x<errno> b=0x<调用方返回地址>` / `CXA_THROW a=.. b=..` 写到 stderr，
  再用 `dlsym(RTLD_NEXT, ...)` **转发给真正的实现**（所以不改变行为）。用宿主 gcc 就能编，
  完全不需要 musl 交叉工具链：

  ```sh
  gcc -shared -fPIC -nostdlib -fno-stack-protector -fno-omit-frame-pointer -O2 \
      -o libthrowtrace.so shim.c     # 只留一个未定义符号 dlsym，由客体 musl 在加载时解析
  ```

  写进镜像 `/usr/lib/libthrowtrace.so`，再给 MC 的进程环境加
  `LD_PRELOAD=/usr/lib/libthrowtrace.so` 即可。**已实测有效**：java 进程的 maps 里出现了
  `LIBMAP 60000000-60003000 /extra/usr/lib/libthrowtrace.so` ⇒ **`LD_PRELOAD` 在 A20OS + Alpine
  客体上工作**，垫片被加载、进程正常启动。

  **但还没抓到那个 throw**：带垫片跑了两轮，**两轮都没有抛异常**——一轮卡在 LWJGL 之后，
  另一轮**越过了 `Using optional rendering extensions` 之后才卡住**（都没有 `THROW_SYSERR`/`CXA_THROW`，
  也没有 `terminate`）。后者很关键，它**修正了本文前面「abort 是确定性的」这个说法**：
  带垫片时出现了「过了那个点也不 abort」的轮次 ⇒ abort 其实是**时序/内存布局敏感**的，
  并且和「卡住」很可能是**同一个根因的两种表现**（线程创建不是抛异常→abort，就是阻塞→卡住）。
  ⇒ 下一步：**多跑几轮**（垫片已在镜像里，重跑很便宜）直到有一轮真的抛——那时 `THROW_SYSERR`
  会直接给出 **errno** 和**调用方返回地址**，再对照同一轮日志里的 `LIBMAP` 行
  （已打印 libc/libstdc++/libgallium/libLLVM 的映射基址）就能定位到是哪个库、哪个函数在抛。

  **垫片再升级一轮（加了 `pthread_create` 拦截）后又拿到三条硬结论**：
  1. **19 次 `pthread_create` 全部成功**：`PTHREAD_ENTER` 与 `PTHREAD_CREATE` 数量相等（各 19），
     返回码全是 `0x0`，**没有一次非零返回、也没有「进了没出来」的悬挂项** ⇒
     那一轮的卡住**与线程创建失败无关**（该轮 MC 既没抛异常，也没有线程创建在阻塞）。
  2. **「卡住」是 CPU 忙等、不是阻塞**：QEMU 的 CPU 占用稳定在 **~75%**（两次采样 75.7% / 75.3%），
     而 MC **~11 分钟没有任何新输出**（停在 `Backend library: LWJGL version 3.3.3+5` 之后再无进展）⇒
     **不是「慢但最终能过」，而是「在 GL 初始化里自旋/活锁、不推进」**——若只是正常编译着色器，
     11 分钟总该有进展或后续输出。
  3. **那一轮最后又被 lwIP panic 干掉了**：这是**第 4 次复现**，签名依旧（`memcpy+0x34e` /
     `ethernet_output+0x52a` / idle 任务，整机 poweroff）。而且这次恰好发生在 MC 处于上面那个
     **CPU 满载的 GL 初始化阶段**，进一步支持「该 panic 是**重负载/竞态相关**」这个判断。

  ⇒ 因此优先级应当调整：**先修那个会整机 poweroff 的 lwIP panic**——它比 MC 的 abort/stall 更严重，
  而且现在 4/8 轮都能撞上，**复现率已经够做内核侧插桩**（例如在 TX 路径对 pbuf 的 alloc/free 加
  `kerr` 记账，复现时看是哪个 pbuf 被提前释放）；之后再回头处理 GL 初始化的自旋
  （下一个便宜的手段：在垫片里再拦 `dlopen`，看自旋前最后加载的是哪个库/哪个库的初始化在转）。

- **内核侧 pbuf 双重释放探针（做过、已还原）**：照上面那个计划插了桩。有意思的是 lwIP 里**本来就有**
  一个 `p->ref == 0`（双重释放）检测块，但它被
  `#if defined(CONFIG_BOARD_LS2K1000) && defined(CONFIG_COOPERATIVE_BOOT)` 关掉，而且里面用的是
  RISC-V 的 `move %0, $sp` 内联汇编，在 x86_64 上根本编不过。于是把它**改成 x86_64 可用**
  （用 `__builtin_frame_address(0)` 取 sp），打印前缀换成 `[PBUFDBL]`；另外在 `ref == 0` 的释放路径上
  **把即将释放的 pbuf 的 payload 毒化**为 `0x6000dead`，这样「释放后仍被使用」会以一个可识别的地址崩出来。

  **结果：2 轮都是 `[PBUFDBL]` 0 次、并且这 2 轮压根没复现 panic**（`0x6000dead` 也没出现），
  而此前 8 轮里有 4 轮能撞到 panic ⇒ **这 2 轮没有信息量**。探针只有在**真的 panic 的那一轮**才能判定：
  要么崩在 `0x6000dead`（⇒ 释放后被使用），要么打印 `[PBUFDBL]`（⇒ 存在双重释放）。

  **探针已全部还原**（`git checkout -- kernel/external/lwip/src/core/pbuf.c`，`git diff` 为空、
  原 `#if defined(CONFIG_BOARD_LS2K1000)` 守卫已复位并重新编译过），因为它在释放路径上修改 pbuf 内容、
  不能留在树里。**下次要判定这个 panic，就照上面这段重新插桩，然后一直重跑到复现为止**。

- **【探针在「确实 panic 的那一轮」生效了——本轮最硬的结论】**：把探针重新插上后又跑了 4 轮
  （3 轮没复现、第 4 轮复现了 panic），于是拿到了决定性数据：

  1. **`p->ref == 0` 的双重释放一次都没发生**：`[PBUFDBL]` 计数 = **0** ⇒ **双重释放被排除**。
  2. **毒化值 `0x6000dead` 一次都没出现**（计数 = 0）⇒ 出问题的 pbuf **不是**经由 `pbuf_free()` 的
     ref==0 路径释放的 ⇒「已释放的 pbuf 被再次使用」也被排除。而 `ethernet_output` 里崩溃的那个
     memcpy 是**往 payload 写**，所以那个 pbuf 是**活的、但 payload 是野值**。
  3. **BADV 的形状是关键线索**：这次 `0x71bd3000`，上次 `0x71bc3000`——**正好差 `0x10000`，即一个
     64 KiB 步长**；而 `kernel/mm/elf.c` 的 ASLR 正是 `ASLR_BITS 11` 且 `bits << 16`（64 KiB 步长）
     ⇒ 这个坏地址是**带 ASLR 偏置的用户态映射基址派生出来的**，也就是一个**用户态地址**
     （`MMAP_BASE_ADDR 0x60000000` 之上的 mmap 区）⇒ **内核把用户态指针当成 pbuf 的 payload 在写**。
  4. 崩溃 PC 与上次完全相同（`0xffff8000002103ee` = `memcpy+0x34e`）、backtrace 也相同
     （`ethernet_output+0x52a`、idle 任务）⇒ 确认是**同一个 bug**，不是随机损坏。

  ⇒ **修正前面一个结论**：之前写的「所有发送路径都拷贝、没有 `PBUF_REF` 指向用户态内存」，
  检索范围只覆盖了 `kernel/net/`，**范围不够**。现在有直接证据表明**某处确实把用户态缓冲以引用
  方式交给了 lwIP**（或者某个活 pbuf 的 payload 被写坏成了一个用户态地址）。
  **下一步就在这个范围内重新找**：`kernel/net/` 之外的 lwIP 调用者（尤其 raw socket /
  `netconn` 风格的接口）、lwIP 内部所有 `PBUF_REF` / `pbuf_alloc_reference` 使用点、
  以及 ARP 排队（`etharp_query`）是否以**引用**方式挂包而不是拷贝。

  另外记一个死胡同：**`/proc/<pid>/stat` 的字段 30（kstkeip）在 A20OS 上是空的**（取不到任何
  指令指针），所以别指望用 procfs 采样自旋位置；但同一条记录里的 utime/stime（字段 14/15）是**有效**的，
  可以用它来确认某个进程在烧 CPU（这轮就看到一个 state=`R`、utime 持续增长的进程）。

- **再一轮全仓检索：把「用户态指针进 pbuf」也排除了，并给出新的定性**：

  1. **没有任何代码会创建「引用型」pbuf**：搜 `PBUF_REF` / `PBUF_ROM` / `pbuf_alloc_reference` /
     `pbuf_alloced_custom`，**A20OS 自己的代码里 0 处命中**；vendored lwIP 里也只有**头文件里的声明**，
     没有任何 `.c` 使用点 ⇒「有人把用户态缓冲以引用方式塞进 pbuf」**不成立**。A20OS 自己的
     `pbuf_alloc` 总共只有 3 处（`lwip_stack.c:331` 的 RX 用 `PBUF_POOL`、
     `socket_inet.c:1009/1042` 用 `PBUF_RAM`），且全部配 `pbuf_take()` **拷贝**。
  2. **也没有「延迟 TX」机制**（`tx_defer` / `pending_tx` / `txq` 等在 `kernel/net/` 里 0 命中）。
  3. 结合上一段的探针结论（**无双重释放、毒化值从未出现**）⇒ 那个坏 pbuf **从未被释放**，
     它是**活的**，只是 `payload` 字段**在使用中被改掉了** ⇒
     **这是「活着的内核对象被写坏」，不是 pbuf 生命周期错误**。
  4. 旁证（不要据此下结论）：panic 的寄存器转储里有 `0xcafe03dbfe59b560` / `0xcafe03dbfefdcd40`
     这类**非规范地址**（4 级分页下不合法）且共享高 48 位；但这类转储本身已被证明不可靠。
     内核里确实有毒化常量：`kernel/mm/slab.c` 的 `BIG_CANARY 0xCAFEBABE`、
     `kernel/ipc/a20_channel.c` 的 `data_len = 0xDEADBEEF` 哨兵。

  ⇒ **定性更新**：这个 panic 很可能**不是网络栈自身的 bug，而是内核内存损坏的受害者**。
  这与 `known-issues.md` 里那条一直悬而未决的「多线程进程的匿名内存偶发被写坏」属于同一类问题，
  也顺带解释了为什么 `tumblerd` 会以 `code=1` 的野指针 SIGSEGV、为什么 JVM 会抛出内容为空的
  `std::system_error`。**下一步方向应当转向「内核里谁在越界写/悬垂写」而不要在 lwIP 里继续找**：
  可复用现成的 `BIG_CANARY` 思路给可疑内核对象加围栏并周期性校验，复现时即可抓到越界写者。

- **`dlopen` 探针：结论是「没有任何库卡在加载里」**

  垫片加了 `dlopen` 拦截，成对打印 `DLOPEN_ENTER` / `DLOPEN_DONE`（带 `t=` 单调毫秒与 `tid=`），
  重新注入镜像后实测跑通：探针标记 `SDDL0/1/2` 全部出现，31 次 dlopen **全部成对**。

  ```
  t=58748  tid=148  #1  DLOPEN_ENTER /usr/lib/jvm/java-21-openjdk/lib/server/libjvm.so
  ...
  t=95083  tid=236  #95 DLOPEN_ENTER libGLX.so.0
  t=95086  tid=236  #97 DLOPEN_ENTER libGL.so.1
  t=105366 tid=1460 #99 DLOPEN_ENTER /usr/lib/jvm/java-21-openjdk/lib/libextnet.so
  ENTER=31  DONE=31
  ```

  - **31 个 ENTER 全有对应 DONE ⇒ 没有任何库的加载/初始化卡住**。「某个库的 initialiser 在自旋」
    这个假设**被否掉**：卡点不在 `dlopen` 里。
  - **Mesa 的 DRI/gallium/LLVM 一个都没被 dlopen**：镜像里明明有 `libgallium-25.2.7.so`、
    `libLLVM.so.21.1`、`libstdc++.so.6`，31 次里却没有它们 ⇒ 带垫片这一轮里 MC 在
    **Mesa 加载驱动之前**就停住了，比不带垫片时更早（不带垫片时它已打出
    `Using optional rendering extensions` 才在 Mesa 的 LLVM 路径 abort）。**垫片把失败点提前了**，
    所以这份清单只覆盖到 GL 之前。
  - 顺带得到**完整的 GL 前依赖清单**（有用）：libjvm / libjimage / libjava / libjsvml / libnio /
    libzip / libmanagement(_ext) / libnet / libverify / JNA 临时文件 / libudev /
    `/tmp/lwjgl_root/3.3.3+5/x64/{liblwjgl.so,libglfw.so}` / X11 全家（libX11、libXxf86vm、libXi、
    libXrandr、libXcursor、libXinerama、libX11-xcb、libXrender、libXext）/ `libGLX.so.0` /
    `libGL.so.1` / libextnet。

- **【修正前面的结论】「~75% CPU 自旋」是误判 —— MC 其实是阻塞（state=`S`，几乎不吃 CPU）**

  同一轮里采样 `/proc/<pid>/stat`：java 进程 **state=`S`**，`utime=7`、`stime=17`（HZ=100，
  合计不到 0.25 秒 CPU）。QEMU 整体累计 34.6%（4:38 里占 1:36）——这批 CPU 是**软件渲染的桌面**
  （pixman compositor + swaybg 持续重绘）烧的，**不是 MC**。所以本文前面写的
  「在 GL 初始化里 CPU-忙自旋/活锁、不推进」**不成立**：MC 是**卡在一个等待上**，不是自旋；
  这也解释了它「11 分钟零输出」却始终停在 `S` 态。截图佐证：桌面完全健康（壁纸/图标/面板/dock/鼠标
  都在），**屏上没有 MC 窗口** —— 这一轮它根本没走到建窗。

  日志里另有 MC authlib 的网络异常栈（`MinecraftClient.readInputStream` →
  `HttpURLConnection.getResponseCode`，客体没有可用网络），与本问题无关，属既有噪音。

  ⇒ 下一步换成**「找它在等什么」**：采样器里加 `/proc/<pid>/syscall`（直接打出阻塞的 syscall 与
  参数）和 `/proc/<pid>/wchan`，复现阻塞时就能区分 futex / poll / read。
  （`/proc/<pid>/stat` 字段 30 为空、`wchan`/`stack` 是桩，只剩 `syscall` 可试。）

- **harness 踩到的坑（已修，记下来省得再踩）**：`start-desktop-components.sh` 的**最后一行是
  `wait`**，而无参 `wait` 会一直等那些长驻桌面组件。用 `cat >>` 追加的探针块会落在 `wait`
  **之后**、永远执行不到 —— 现象极具误导性：桌面与壁纸都完全正常，但探针一行输出都没有
  （本轮第一次跑就是这样，靠截图才定位）。正确做法是把探针块插在最后的 `wait` **之前**。

- **【本轮最硬的结论】抓到那个异常了：`std::system_error`，抛在 OpenAL/声音引擎初始化处**

  把垫片的 `__cxa_throw` 加固后（见下一条），这一轮终于在**崩之前把异常打了出来**：

  ```
  t=97971 tid=240 #201 DLOPEN_ENTER /tmp/lwjgl_root/3.3.3+5/x64/libopenal.so
  t=97994 tid=240 #203 CXA_THROW_TYPE St12system_error
  t=98014 tid=240 #204 CXA_THROW a=0xd2d63168 b=0xcfc17958
  （全轮 81 个 dlopen：ENTER=81 DONE=81）
  ```

  - **异常类型实测就是 `std::system_error`**（`St12system_error` 是它的 mangled name）——
    与最初不带垫片时那句 `terminate called after throwing an instance of 'std::system_error'`
    完全对上，**确认无误**。
  - **抛点在 `libopenal.so` 加载后约 23 毫秒**，也就是 **OpenAL / 声音引擎初始化**阶段；
    此刻 MC 已经打出 `Using optional rendering extensions`（GL 上下文已建成）。
    ⇒ 它**不在 Mesa 的 LLVM 路径里**；之前「softpipe 能绕过」的现象与这条并行存在，
    但抛异常的直接现场是**音频**。
  - `a=0xd2d63168` 是 `__cxa_throw` 的**调用方返回地址**（真正那条 `throw` 指令所在处）。
    要定位到具体库/函数，只需在此时把 `/proc/<pid>/maps` 打出来对照这个地址。
  - 只有 `CXA_THROW`、没有 `THROW_SYSERR` ⇒ 是代码里直接 `throw std::system_error(...)`
    （走 `__cxa_throw`），不是经 `std::__throw_system_error` 辅助函数。

- **垫片自己先崩过一轮，必须先修才不会误判成 A20OS 的锅**

  前一版垫片装上就 SIGSEGV，而且**崩在它自己身上**：

  ```
  SIGSEGV (0xb) at pc=0x000000006000187c
  # Problematic frame:
  # C  [libthrowtrace.so+0x187c]  __cxa_throw+0x7c
  ```

  根因：`__cxa_throw` 拦截器里为了多打一层调用者用了 `__builtin_return_address(1)`，
  它会顺 frame pointer 链走出我们的栈帧去读调用方的栈；而 MC 的 JVM **不维护 rbp 链**，
  于是读到垃圾地址再 deref ⇒ 垫片自己 SIGSEGV、把进程打死，**把真 bug 完全盖住**
  （现象：一行 `CXA_THROW` 都没打出来 —— 因为 `return_address(1)` 是在**求值实参**时就崩的）。
  修法：只保留 `__builtin_return_address(0)`（读的是自己那帧的 `[rbp+8]`，一定有效），
  并顺手把异常类型名读出来。**教训：拦截器里不要用 `return_address(N>0)`。**

- **`/proc/<pid>/syscall` 在 A20OS 上也是空的**（又一条 procfs 桩）

  想用它看 MC 阻塞在哪个 syscall，结果 78 次采样全是 `syscall=[]`；`wchan` 只有 `do_wait` 和 `0`。
  ⇒「它在等什么」这条线靠 procfs 走不通，得换手段（例如在垫片里拦 futex/poll/read 入口）。

- **另一个可疑现象（只记录观察，不做解释）：僵尸子进程无人回收**

  这轮 java 进程（pid=152）死后连续 64 次采样都是 `st=Z`（僵尸），而 launcher 的 `wait "$MC"`
  始终没返回（`=== SW exit ===` 一行都没出现）。按理 `wait` 对已是僵尸的子进程应立即返回，
  这可能是等待/回收路径的真问题，也可能是我这层嵌套 subshell 的语义影响。
  **留作独立线索，别和上面那个异常混为一谈。**

- **崩溃地址旁证**：JVM 报的 `pc=0x47c0602b`，同轮内核日志另有一处
  `sepc=0x4329602b stval=0x10`（pid=129，桌面阶段）——两处低 12 位相同（`602b`）、
  高地址不同（ASLR 偏移），且 `stval=0x10` 是**典型的 NULL+0x10 解引用**。
  指向「某个重定位库里的同一段代码踩空指针」，但仍需 maps 才能定位到具体库。

- **【定位到库了】抛这个 `std::system_error` 的就是 `libopenal.so`（OpenAL 声音引擎）**

  在垫片里给 `__cxa_throw` 加了「读 `/proc/self/maps`、把调用方地址翻译成所属库」，
  一轮就命中：

  ```
  t=94274 tid=238 #203 DLOPEN_ENTER /tmp/lwjgl_root/3.3.3+5/x64/libopenal.so
  t=94288 tid=238 #205 CXA_THROW_TYPE St12system_error
  t=94290 tid=238 #206 CXA_THROW a=0xd291e168 b=0xcfcec958
  t=94291 tid=238 #207 MAP_HIT /extra/tmp/lwjgl_root/3.3.3+5/x64/libopenal.so
  ```

  - **`a=0xd291e168`（即 `__cxa_throw` 的调用方地址）落在 `libopenal.so` 的映射内** ⇒
    这个 `std::system_error` 是 **OpenAL 自己抛的**；抛点在 `libopenal.so` 加载后约 14 毫秒，
    也就是 **OpenAL 初始化（打开音频设备那条路径）**。
  - MC 的死因链因此是：**OpenAL 在 A20OS 上初始化失败 → 抛 C++ `std::system_error` →
    没人接 → 进程被打死**（`terminate`/abort；另一轮则在展开路径上再踩一次空指针，
    表现为 SIGSEGV）。这解释了它为什么总发生在 `Using optional rendering extensions` 之后，
    也解释了之前盯着 Mesa 为什么一直没进展 —— **直接现场在音频，不在图形**。
  - 顺带确认垫片加固有效：`MAP_HIT` 正常打出，这一轮没有 `libthrowtrace.so+...` 的自崩。

  ⇒ **下一步（很便宜，可能直接让 MC 过关）**：OpenAL Soft 支持用「空设备」后端绕开真实音频设备
  —— 给 MC 的环境加 `ALSOFT_DRIVERS=null` 再跑。预期：`CXA_THROW_TYPE St12system_error`
  消失，MC 能越过这个点继续往下走。这比继续啃图形栈便宜得多，且能立刻证实/证伪音频这条链。

- **`ALSOFT_DRIVERS=null` 没能救它；但这一轮把崩溃点也定位到了 musl 的 ld.so**

  给 MC 环境加 `ALSOFT_DRIVERS=null`（OpenAL Soft 的空设备后端）再跑，结果**没有变化**：

  ```
  t=93459 tid=239 #203 DLOPEN_ENTER /tmp/lwjgl_root/3.3.3+5/x64/libopenal.so
  t=93467 tid=239 #204 DLOPEN_DONE   a=0xd2035030
  t=93484 tid=239 #205 CXA_THROW_TYPE St12system_error
  t=93486 tid=239 #206 CXA_THROW      a=0xd2ce3168
  t=93489 tid=239 #207 MAP_HIT        /extra/tmp/lwjgl_root/3.3.3+5/x64/libopenal.so
  ```

  - 异常照旧、类型照旧、库照旧 ⇒ **不是「换空设备后端就能绕过的那个音频后端问题」**。
  - 同时把 `pthread_create` 的返回码全查了：**21 次全部 `rc=0x0`**，而且从 t=90876 到
    抛出的 t=93484 **没有任何一次线程创建** ⇒ 「`std::thread` 构造失败抛 `std::system_error`」
    这个假设**同样被否定**。（`std::system_error` 最常见的两个来源就是线程创建失败与
    锁/`call_once` 一类；现在两个都排除了。）

- **【关键】崩溃帧指向 `ld-musl-x86_64.so.1+0x4602b` —— 反复出现的 `602b` 之谜解开了**

  这一轮的 JVM fatal error 终于写出了具体库：

  ```
  #  SIGSEGV (0xb) at pc=0x000000004730602b, pid=151, tid=239
  # Problematic frame:
  # C  [ld-musl-x86_64.so.1+0x4602b]
  ```

  前面几轮那几个「低 12 位都是 `602b`、高位随 ASLR 变」的地址（`0x47c0602b`、`0x4329602b`、
  `0x4730602b`）**全部是 musl 动态链接器里的同一个偏移**（`ld-musl-x86_64.so.1+0x4602b`）——
  不是「某个重定位库」，而是**动态链接器本身**。

  ⇒ 死因链更新为：**`libopenal.so` 抛出 `std::system_error` → 异常展开进入 musl 的 `ld.so`
  （`_Unwind_Find_FDE` / DSO 查找那条路径）→ ld.so 自己踩空指针崩溃**。
  同轮内核日志有一行佐证：`ADE/ALE: pid=239 sepc=0x60be1318 stval=0x14c834000 code=1`，
  其中 `stval=0x14c834000` 明显不是正常的用户地址。

  这条链的价值：故障发生在**「展开一个 `dlopen` 进来的模块的异常」这条通用基础路径**上，
  与 MC 的业务逻辑无关。它很可能同时解释 `known-issues.md` 里那些别的受害者
  （mpv 的 Lua 线程、`tumblerd` 的野指针、JVM 那个 `what()` 为空的 `std::system_error`），
  并与「多线程进程的匿名内存偶发被写坏」那条老线索同源。

  ⇒ 下一步（比继续调 MC 更值）：**盯 musl 的异常展开 + `dlopen` 模块的 DSO 查找**。
  做法：写一个最小 C++ 程序，在客体里 `dlopen` 一个自定义 `.so` 并 `throw`，看能否**独立复现**
  `ld-musl+0x4602b`。若能独立复现，这就成了一个不依赖 Minecraft 的干净最小用例。

- **把 `ld-musl+0x4602b` 反汇编看了：那是一条「自指针一致性检查」，`rax=NULL` 时正好踩 `0x10`**

  从镜像里把 `ld-musl-x86_64.so.1` 抠出来（`debugfs dump`），在宿主上反汇编出错的那条指令：

  ```
  46023:  48 8b 47 f0   mov  -0x10(%rdi),%rax     # 取链表头
  46027:  48 8d 4f f0   lea  -0x10(%rdi),%rcx     # 期望的自身指针
  4602b:  48 39 48 10   cmp  %rcx,0x10(%rax)      # ← 崩在这里：读 [rax+0x10]
  4602f:  74 01         je   ...                   # 一致则继续
  46031:  f4            hlt                        # 不一致则 a_crash()
  ```

  - 这是一段**双向链表的自洽性检查**（`if ([rax+0x10] != rdi-0x10) a_crash();`）。
  - 它**解释了 `stval=0x10`**：当 `rax == NULL` 时，`cmp %rcx,0x10(%rax)` 读的正是地址 `0x10`
    ——与前面那轮内核日志 `stval=0x10` 完全吻合；另一轮 `stval=0x14c834000` 则是同一个
    `rax` 变成了垃圾大地址。两次是**同一条指令、同一种失效**。
  - 所在函数从 `0x45eca` 起（`push %r15; mov %rsi,%rdx; and $~0xf,%rsi; push %r14; sub %rdi,%rdx`，
    做 16 字节对齐与长度计算，周围多处 `hlt` 断言）——形似**分配器/内存管理**那段代码，
    但该库**已被 strip**（`nm` 报「无符号」）。

- **这条线现在做不下去的原因（记为阻塞，不是没做）**

  想给它点名、以及想写「最小 C++ 复现程序」，都卡在同一个前提上：

  1. **客体里没有任何 C++ 编译器**：xfce 镜像的 `/usr/bin` 里没有 `gcc`/`g++`；带 `gcc` 的 world
     只有 `devtools`/`devtools-smoke`（而且**都没有 `g++`**），现成的 devtools 镜像
     **全是 riscv64**（`build/images/devtools-riscv64.img` 等），**没有 x86_64 版**。
     要编译「`dlopen` 自定义 `.so` 再 `throw`」的最小用例，就得改 world 加
     `g++`（可能还要 `musl-dev`/`libstdc++-dev`）⇒ **需要联网拉包 + 重建镜像**。
  2. **没有符号可查**：`ld-musl-x86_64.so.1` 已 strip，它的 `.gnu_debuglink` 指向
     `ld-musl-x86_64.so.1.debug`，而镜像里**根本没有 `/usr/lib/debug`**（`musl-dbg` 未装），
     所以也无法在客体侧符号化。

  ⇒ **解锁条件（任选其一，都需要一次基础设施决定）**：
  （a）给镜像加 `g++` 与调试信息包（`musl-dbg`），再跑最小 C++ 用例；
  （b）在宿主侧备好能链 musl 的 C++ 交叉工具链（当前没有——这正是当初垫片要 `-nostdlib`
  手写的原因）。
  在此之前，这条线只能在「反汇编 + 现有 MC 日志」的范围内推进。

- **【绕过了那个阻塞】用「无头文件的 freestanding C++ + 宿主 g++ + 垫片驱动」做出了最小用例；
  结论是：跨 `dlopen` 模块的 C++ 异常展开**本身**是好的**

  前面把这条线记成阻塞（客体没有 C++ 编译器）。其实可以绕开：最小用例只需要 `throw`/`catch`
  一个**基本类型**，不需要任何头文件；而 `__cxa_*`/`_Unwind_*`/`_ZTIi` 这些未定义符号由客体
  自己的 libstdc++/libgcc_s 在加载时解析。于是用**宿主的 g++** 就能编出来：

  ```
  g++ -shared -fPIC -nostdlib -fexceptions -fno-stack-protector -O2 \
      -o libthrowtest.so throwtest.cpp     # 无 NEEDED，未定义符号留给客体解析
  ```

  再由垫片（它本来就被 `LD_PRELOAD` 进 MC 的进程）在**第一次 `dlopen` 时**做自检：
  先用 `RTLD_GLOBAL` 载入 libstdc++ 与 libgcc_s，再 `dlopen` 测试库、`dlsym` 三个函数并调用。
  实测（tid=148，就是 MC 的 java 进程）：

  ```
  SELFTEST_DLOPEN a=0x2e94ab0            # 测试库加载成功
  SELFTEST_SYM    a=0x60326070 b=0x603260af
  MAP_HIT /extra/usr/lib/libthrowtest.so # 我的 maps 查找同时在正常工作
  SELFTEST_CAUGHT a=0x2a                 # 42 —— 被 dlopen 的模块里 throw/catch 跨边界：正常
  SELFTEST_NESTED a=0xe                  # 14 —— 重抛 + 析构：正常
  SELFTEST_UNCAUGHT_CALL
  terminate called after throwing an instance of 'int'
  Aborted (core dumped)                  # 未捕获异常路径也**教科书式正确**
  ```

  - **两个子用例都干净**：捕获式跨模块展开 OK；未捕获异常正确走到 `terminate` → `abort`
    （SIGABRT）。**都没有出现 `ld-musl+0x4602b`。**
  - ⇒ **「在一个 `dlopen` 进来的模块里抛 C++ 异常并展开」这件事本身在 A20OS 上是好的**，
    并不是通用的展开器/DSO 查找逻辑坏掉。MC 那个崩溃还需要额外条件。

  ⇒ MC 那条崩溃的差异点只可能在这几个方向（按可疑度排序）：
  1. **抛点不在主线程**（MC 是 Render thread，tid≈238），而本次自检在主线程；
  2. **展开时要穿过 JVM 的 JIT/匿名映射帧**（没有 FDE 的帧）以及 MC 那一大堆模块的布局；
  3. 或者根本不是展开逻辑问题，而是**它遍历的那个链表已经被写坏**（`rax` 为 NULL/垃圾，
     见前面反汇编）——即回到「内存被写坏」那条线：与 `kernel/` 侧「多线程进程匿名内存偶发
     被写坏」、以及那个整机 poweroff 的 panic 可能是同一个根因。

  顺带修正一处**我自己的误判**：`throwtest_nested` 我期望 1014，实测 14 —— 这是我对 C++ 语义
  判断错了，不是 bug：`return r;` 在**局部对象析构之前**取值，返回 14 完全正确。

  再记一条 harness 经验：`dlopen` libstdc++ 时**必须带 `RTLD_GLOBAL`（0x101）**，否则它的符号
  不进全局作用域，测试库会以 `Error relocating ...: _ZTIi: symbol not found` 加载失败
  （第一轮就是这么栽的）。

- **【补完最小用例】「非主线程」这个条件也测了：同样干净 ⇒ 通用展开逻辑没问题，
  那个 ld.so 崩溃更像是「它遍历的链表已经被写坏」**

  前面的自检全在主线程，而 MC 是在 Render thread 上抛的；加上崩溃点那段 ld.so 代码遍历的是
  **每线程**的链表结构，所以补了「先 `pthread_create` 起线程、在线程里 throw/catch」两个变体：

  ```
  SELFTEST_THREAD_CAUGHT a=0x5            # 5 —— 非主线程里跨模块 throw/catch：正常
  SELFTEST_THREAD_UNCAUGHT_CALL
  terminate called after throwing an instance of 'int'
  Aborted (core dumped)                   # 非主线程未捕获：教科书式 terminate/abort，干净
  ```

  - **非主线程的跨模块展开也是好的**，未捕获路径同样正确 terminate。**依然没有 `ld-musl+0x4602b`。**
  - 至此最小用例共覆盖 5 种情形 —— 主线程捕获、主线程重抛+析构、主线程未捕获、非主线程捕获、
    非主线程未捕获 —— **全部行为正确**。
  - ⇒ **「A20OS 上跨 `dlopen` 模块的 C++ 异常展开」这条通用路径已被证明可用**；
    MC 那个崩溃不是展开器/DSO 查找的逻辑 bug。

  ⇒ 剩下的差异只有三种可能：展开时要穿过 **JVM 的 JIT/匿名映射帧**（没有 FDE 的帧）、MC 那套
  复杂模块布局，或者——**那段 ld.so 代码遍历的链表 `rax` 本来就是 NULL/野值**（见前面反汇编），
  也就是**内存被写坏**，而不是展开逻辑出错。第三种现在**最站得住**：
  1. 展开逻辑已被 5 个用例证明是好的；
  2. 崩溃指令本身是「链表自指针一致性检查」，`rax` 为 NULL/野值 ⇒ 它读到的是一张**已损坏的表**；
  3. `kernel/` 侧本来就有「多线程进程匿名内存偶发被写坏」这条未决线索，而那个整机 poweroff 的
     TX panic 也被定性为「活着的内核对象被写坏」。

  ⇒ **下一步收敛到「谁在写坏内存」**：给可疑内核对象加 `BIG_CANARY` 围栏并周期性校验（做法前面
  已记），以及/或者把同一套最小用例放到「多线程 + 大量 `dlopen`」的压力下跑，看能否把那张表打坏。
  这比继续深挖 musl 的 ld.so 更值得做。

  再记一个我自己踩的坑（留给后来者）：`throwtest.cpp` 的注释里写了
  `__cxa_*/_Unwind_*/_ZTIi`，其中的 `*/` **提前闭合了块注释**，后面被当成代码、编译直接失败
  （`_Unwind_ does not name a type`）；而旧 `.so` 还留在镜像里，表面上看「自检跑了、只是线程变体
  没执行」。**C/C++ 注释里永远不要写 `*/`。**

- **`LP_NUM_THREADS=0` 不能绕过这个 abort**：用它（关掉 llvmpipe 的**光栅化**多线程）跑 MC，
  `std::system_error` **照样抛**、`rc=134` 照样退（t+60 时进程已经没了，`=== LP gone t+60 ===`）。
  ⇒ 失败的**不是 llvmpipe 自己的 rasterizer 线程池**，更可能是 **LLVM 内部**的线程
  （`llvm::thread`/ThreadPool —— llvmpipe 的 JIT 正是走 LLVM）；这与「完全不进 LLVM 的 softpipe
  能避开它」一致。下一步就是抓上面那个 throw 的 caller 与 errno。

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

- **已修（`d8a0c12b`）：AF_UNIX channel 发送路径按「每块」唤醒对端** —— 这就是
  `glfwCreateWindow` 卡死的根因。`socket_unix.c` 的 channel 分支只在**整笔写返回之后**才唤醒
  `dst->read_waitq`，而 `unix_ch_send()` 要按 `A20_CH_MAX_DATA` 分块、并且会在通道滿时
  **在 `a20_channel_send()` 里 park**。于是：写方已经往通道里放了字节、却因为还没返回而没有发唤醒；
  而读方如果在这之前就已经 park 在 `poll()`，它等的正是那个唤醒 —— 读方等数据、写方等空间，
  而空间只有读方能腾出来：**互锁**。这同时解释了为什么它时序相关，以及为什么「让
  `unix_ch_send()` 改成短写」没用（阻塞点在 `a20_channel_send()` 内部，不是 `WOULD_BLOCK` 返回）。
  修法：每成功入队一块就唤醒一次对端。
  **验证**：在**未追踪**（此前这个卡死每次复现、最多只走到 `Backend library: LWJGL version 3.3.3+5`）
  的配置下，现在 4 秒后就打出 `Using optional rendering extensions: ...`，加载窗口在屏上。
  **这个卡死确实被修掉了，但 MC 又停在下一个点上**：之后它的输出就停了（日志停在 373 行、
  再无新行、无 panic、无 OOM），过了一会儿它的**窗口也消失了** —— 即建窗与 GL 初始化都过了，
  进程随后是**静默退出**（没有 `Stopping!`、没有 JVM 崩溃报告、内核也没有 fault）。
  所以「卡在 `glfwCreateWindow`」这一条已经结束，剩下的是**另一个**问题：MC 在渲染器起来之后
  无声消失。  候选方向（都还没查）：x86_64 已知的每线程状态 bug（`known-issues.md` 里 mpv 的
  Lua 线程就栽在这上面，而 MC 是重度多线程）、或渲染器初始化里的某个信号/异常路径。
  **主菜单仍未见到。**

- **新失败的根因已定位：不是"静默退出"，是 SIGABRT（实测 `rc=134`）**。launcher 的退出码
  实测为 **134 = 128+6 = SIGABRT**，存活时间线也吻合：t+30s 还活着（`wchan=do_wait`），
  t+60s 已经没了；而日志在 `Using optional rendering extensions` 的**下一行**就是：

  ```
  terminate called after throwing an instance of 'std::system_error'
    what():  No error information
  ```

  即某个 C++ 调用抛了 `std::system_error` 且没人接 → `std::terminate()` → `abort()` → 134。
  `what()` 是 `No error information` 很关键：那是 **musl 的 `strerror` 兜底字符串**（errno 没被识别），
  而在这类上下文里 `std::system_error` 最典型的来源就是 **`std::thread` 构造失败**
  （`pthread_create` 返回错误）。MC 1.21 在渲染器起来之后正好要起 `Worker-Main-*` 资源加载线程。
  ⇒ **下一步就查这里**：把 `pthread_create`/clone 的失败（以及失败时的 errno）打出来，
  确认是不是线程创建失败，再顺着 x86_64 每线程状态那条线查下去。
  （同轮日志里 `FATAL: pid=127 signal=11 ... comm=tumblerd` 是**另一个**既有问题——
  `tumblerd` 的 SIGSEGV，与 MC 无关。）

  **这和 `known-issues.md` 里那条既有线索是对得上的**：那条（「java 退出码 255 / mpv 偶发崩溃」）
  已经把内核的**核心线程/内存/退出机制逐项探过并排除**——并发线程互踩缓冲 5/5 干净、每线程 FS/TLS
  4/4 独立、`exit_group` 各形态（含非 leader 触发）5/5 正确、142MB 文件 read/mmap 8/8 一致——
  **剩下唯一站得住的怀疑是「多线程进程的匿名内存偶发被写坏」，方向是线程生命周期
  （线程退出/reap、内核栈、per-thread trap 帧归属）**，并明确写了「会影响长跑的 Java 游戏」。
  MC 这次的 abort 正落在这个圈里。另外 `what(): No error information` 也正是 **musl 对
  errno=0（或未识别 errno）的兜底串**，所以「错误码是 0」本身也是一条线索。

  **下一步（按顺序）**：① 先确认异常来源——在客体里给 `clone`/`pthread_create` 加失败日志（带 errno）
  跑一次 MC；② 若线程创建没失败，就按上面那条既有线索查线程生命周期与 per-thread trap 帧归属；
  ③ 用 `mpv --vo=null` ×N 作对照复现器（它更容易复现同一个 bug）。

- **仍未处理**：上面那个 `ethernet_output` 野指针 panic（**已复现第二次**，签名一致；AF_PACKET
  TX 与「用户态缓冲进 pbuf」两条假设均已排除，剩下的是异步排队的 pbuf 所有权问题）。

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
