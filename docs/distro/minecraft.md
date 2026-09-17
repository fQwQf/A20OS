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

- **仍阻塞（修复后暴露出的真正崩溃）**：JVM 现在干净地崩溃在 `pc=0x0`（跳转到 NULL），
  `hs_err` 显示当前线程 `JavaThread "main"` 正在算 `sun.security.provider.SHA5$SHA384`
  （jar 清单的摘要校验），寄存器 `RIP=0`。用 `-XX:-UseSHA` 关闭 SHA 内联**没有帮助**，
  说明不是 SHA 内联 stub；更像 JIT 出来的代码跳到了空指针（或返回地址被写坏）。
  这正是本文档另一条「多线程匿名内存偶发被写坏」的形态，需要单独继续查。

- **另一个确认的 ABI 缺口（非本次崩溃主因，但会破坏信号语义）**：`hs_err` 报
  `bad uc->uc_mcontext.fpregs: 0x0`——A20OS 的 `arch_sigcontext_t` 把 `fpu[512]`
  **内嵌**在 sigcontext 里，而 Linux 的 `ucontext.uc_mcontext` 在同样位置是
  **指向 fpstate 的指针**（`gregs[23]` + `fpregs`）。布局不一致，读 ucontext 的
  JVM（隐式空指针检查和崩溃报告都读它）会读错偏移。要彻底修，得把
  `arch_sigcontext_t`/`arch_ucontext_t` 改成与 Linux/glibc 一致。


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
