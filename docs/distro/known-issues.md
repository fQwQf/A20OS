# 已知问题与排查笔记

桌面目前能起来、能渲染、键鼠可用。本文档只记录**当前状态与排查提示**（症状、是否已解决、去哪看），不记录实现过程。

## 一、已解决

### 输入：libinput 枚举不到设备
- 症状：libinput udev 后端报 "no input devices"，桌面无输入。
- 提示：两处 ABI 缺口——netlink uevent 缺 `SEQNUM=`（`kernel/net/socket_netlink.c`）；`/dev/input/eventN` 与 `/sys/dev/char/<maj>:<min>` 的 syspath 不一致（devfs + sysfs）。

### 间歇性读路径自死锁
- 症状：高 I/O 阶段偶发整体挂死，串口 `LOCK-STALL` 显示 `owner==waiter`。
- 提示：用户拷贝不得在 `vf->offset_lock` 持锁区内触发缺页；`mutex` 需支持同 task 递归。

### 高 CPU / 桌面空转
- 症状：桌面 idle 时宿主 CPU 100%；组件事件循环空转。
- 提示：三处——x86_64 空闲循环要 HLT；`epoll_ctl` 要按 Linux `file_can_poll()` 拒绝不可 poll 的 fd；DRM/input/epoll 要提供 `poll_sources`，否则 readiness 每 1ms 兜底唤醒。

### x86_64 桌面组件随机 SIGSEGV
- 症状：xfdesktop/panel/thunar/gst 等随机野指针崩溃，仅 x86_64。
- 提示：x86_64 要在上下文切换与信号帧保存/恢复 FPU/SSE。对照 aarch64/riscv64/loongarch64 的 trap 帧已保存 SIMD——缺这层时被抢占线程的 XMM 会被下一个任务覆盖。

### x86_64 信号处理器入口栈对齐错 8 字节（**已修复**：`68abf68f`）
- 症状：**任何使用 SSE 的信号处理器**在入口即 `#GP`，内核反复重投 SIGSEGV，栈逐帧下压直到耗尽；
  串口刷 `ADE/ALE: ... code=1` + `insn@sepc=0x...`（如 JVM 读 jar 时 `0x418739a0` 的
  `movaps [rsp+X], xmm0`）。`code=1` 是 `CAUSE_INSN_FAULT`，x86_64 上**只由 #GP 产生**
  （不是 #AC 对齐异常），`stval` 对 #GP 是**过期 CR2**、不是故障地址。
- 根因：投递路径把帧基对齐到 16（`sp &= ~15`）并让 handler 入口 `rsp` **等于帧基**，
  于是入口 `rsp ≡ 0 (mod 16)`；而 x86_64 SysV ABI 要求函数入口 `rsp ≡ 8 (mod 16)`
  （相当于经 `call` 压了 8 字节返回地址）。handler 序言按标准 ABI 对齐栈做 SSE 溢出
  （`movaps [rsp+X]`），就落在错位 8 字节的地址上 → #GP；#GP 又在 handler 内 → 再投递 → 递归。
- 修法：帧基保持 16 对齐（内嵌 `fxsave64` 区域必须 16 对齐），handler 改在**帧基下方 8 字节**
  进入，sigreturn trampoline 地址放在那儿供 handler 最后的 `ret` 弹出；专用 trampoline 页
  去掉补偿用的 `pushq %rax`。用新 arch 钩子 `arch_signal_handler_sp()` 只对 x86_64 生效。
- 验证：JVM 读 jar 的 `ADE/ALE` 归零、其 SIGSEGV 处理器能正常打印崩溃报告；
  XFCE 桌面起来、组件无 SIGSEGV、无内核 panic。


### thunar 运行一段时间后崩溃
- 症状：GDK 报 `Truncating shared memory file failed: Out of memory`，随后 libwayland 空指针崩溃。
- 提示：wl_shm 池是 memfd，其数据曾用单个连续 kmalloc 缓冲（1024x768x4 需 order-10 连续块），内存碎片化后 ftruncate 失败。现改为按需 order-0 页数组（`kernel/fs/memfd.c`）；页缓存写回/回收仍可继续观察。

### elogind 启动失败
- 症状：`elogind-daemon: Failed to open pin file` 紧接 `Failed to allocate manager object`。
- 提示：elogind 在 manager setup 阶段 pin 自己的 cgroup，路径取自 `/proc/1/cgroup`（`init.scope`）。需先建好该目录与 `/run/elogind`；用**普通 `mkdir`**（busybox `mkdir -p` 会误判 sysfs 父目录为「Not a directory」）。

### thunar 缩略图服务缺失
- 症状：`Thumbnailer1 was not provided by any .service files`，并反复 `Thumbnailer Proxy Failed ... re-initialize`。
- 提示：world 清单补 `tumbler`（提供 `org.freedesktop.thumbnails.Thumbnailer1`）。

### overlay 配置：桌面图标、账号库、polkit 规则权限
- 桌面图标：`xfce4-desktop.xml` 里 `desktop-icons/style` 写成了 `1`（`XFCE_DESKTOP_ICON_STYLE_WINDOWS`＝「最小化窗口图标」），而同一文件又配了 `file-icons`（只有 style `2` 才生效）。改成 `2` 后桌面出现 Home/Trash/Filesystem 图标。
- 账号库：overlay 的 `etc/passwd` / `etc/group` 在 `apk add` **之前**落地，而 xfce world 不含 `alpine-base`，所以它就是账号库基底。原文件是 Debian 风格的极简集合（缺 `nobody`/`daemon`/`wheel`/`games`…，且 `audio=29`/`video=44`/`kvm=47`/`input=290` 等 gid 与 Alpine 不符）。已按 `alpine-baselayout-data` 的规范集合重写（仅 root 的 shell 保留 `/bin/zsh`）；内核 devfs 的设备节点 gid 恒为 0，不依赖这些 gid。
- polkit：Alpine 的 polkit 包把 `/etc/polkit-1/rules.d/`、`/usr/share/polkit-1/rules.d/50-default.rules` 打成 `0700 polkitd:polkitd`；`mkrootfs --usermode` 无法 chown，镜像里变成 `root:root 0700`，polkitd 读不到 → 会话日志出现 `Permission denied` + `Error loading script .../50-default.rules`。`mkrootfs` 现在在 mkfs 的 fakeroot 会话里按 apk 归档声明的 uname/gname 补回属主，会话日志已无该错误。
- 死配置：删除了 X11 时代的 `packages/overlay/xfce-x86_64/` + `packages/world/xfce-x86_64.world`（`instances/xfce-x86_64.toml` 用的是共享的 `xfce` world），以及 devtools overlay 里无人引用的 `poweroff-helper`。
- 两条 XFCE 路径（`tools/a20 run xfce-*` 与 `make distro-run`）原本各带一份 rc.xml/session.conf/udev 规则；现在 `user/rootfs/alpine/build.sh` 也应用 `packages/overlay/xfce`（跳过其 `etc/`），会话层单一来源。

### 点击异常：窗口控制键无效、点击不能聚焦
- 症状：指针能动、面板按钮有响应，但窗口标题栏的最小化/最大化/关闭无响应，点击窗口本体不能聚焦/提升，标题栏也不能拖动。
- 根因：**overlay 的 `rc.xml` 把 labwc 的内置默认绑定整表顶掉了**。labwc 只读一个 `rc.xml`（XDG 顺序里的第一个，即 `/root/.config/labwc/rc.xml`），**不合并** `/etc/xdg/labwc/rc.xml`；而它的内置默认 key/mouse 绑定只在解析结果里 `rc.keybinds` / `rc.mousebinds` **为空**时才安装（`src/config/rcxml.c` 的 `post_processing()`）。overlay 里写了 5 个 keybind 和 1 个 `Root` mousebind，于是 `rc.mousebinds` 非空 → 32 条默认鼠标绑定（`Client/Titlebar/Border Left Press → Focus+Raise`、`Title Left Drag → Move`、`Close/Iconify/Maximize Left Click → Close/Iconify/ToggleMaximize`……）一条都没装；keybind 同理，Alt-Tab/Alt-F4 也一起丢失。面板不受影响，因为它的按钮是客户端 GTK 自绘，labwc 只要有 `ctx.surface` 就照常转发指针事件，与 mousebind 无关。
- 修法：在 overlay 的 `<keyboard>` / `<mouse>` 里显式写 `<default />` 拉回内置默认。必须放在**前面**：`deduplicate_*_bindings()` 保留**后**出现的定义，所以后面显式写的绑定（`W-Return` 等）仍然覆盖默认值。
- 验证：guest 日志从"只有 `create mousebind for Root`、没有 `load default mouse bindings`"变为 `Loaded 32 merged mousebinds` + `Replaced 1 mousebinds`（Root Right 与默认重复被去重）+ `Replaced 1 keybinds`（`W-Return` 覆盖默认的 `lab-sensible-terminal`）；桌面里左键点击 root 触发了 `Handling action 15: ShowMenu`——这条绑定只存在于被拉回的默认表里。
- 涉及文件：`packages/overlay/xfce/root/.config/labwc/rc.xml`（`tools/a20 run xfce-*`）与 `user/rootfs/alpine/overlay/root/.config/labwc/rc.xml`（`make distro-run`），两处同一缺陷。

## 二、未解决

### java 退出码 255 / mpv 偶发崩溃（JVM 本身可用；多线程内存仍待查）
- 症状 A（JVM）：`java -version` 能打印**完整且正确**的版本号，但**每隔一次就 exit 255**（20 次里 9 次失败；另一次 6 次里 3 次失败，模式是 255/0/255/0），**没有任何 SIGSEGV**。
- 症状 B（mpv）：约 1/10 次崩溃（`sepc=0x638a6320 stval=0x8`、`comm=lua/<script>`）；反汇编为 LuaJIT 在 `libluajit+0x53320` 读 `*(G+0x130)` 得到 NULL（GC 链表头被写坏）。
- 已排除：
  - 单线程 LuaJIT 完全正常（6× JIT-on + 4× `-joff` 全过）；
  - 上下文切换已保存/恢复 ra/tp/rbx/rbp/r12-r15/rsp/rflags/cr3 + `fxsave64`/`fxrstor64`（`kernel/arch/x86_64/boot/switch.S`）；
  - 两条 trap 入口（`isr_common` 与 `syscall` 快速路径）都把 `MSR_FS_BASE` 存进 trap 帧 offset 184（`trap.S`），返回时写回（`trap.S` 尾部）；内核态嵌套 trap 走的是 `kernel_trap_handler`（不是 `user_trap_handler`）。
- 仍未定位。已排除的候选（都逐一验证过）：`arch_prctl(ARCH_SET_FS)` 的落点没问题 —— syscall 快速路径同样经 `trap_handler`
  → `user_trap_handler` 设置 `current->trap_ctx`（`trap.S:413`、`trap.c:237`）；内核态嵌套 trap 走 `kernel_trap_handler`；
  switch.S 的寄存器/FPU 保存完整。
- **新证据（按 JVM 子系统分层）**：`java -version` 的退出码在三种配置下分别是
  `plain: 255 0 255 0 255`、`-Xshare:off: 255 255 255 255 255`（**每次都失败**）、`-XX:-UsePerfData: 0 255 0 255 0`（仍交替）。
  说明与 perf-data 无关，而与 **CDS/类数据加载（`lib/modules` jimage 的文件映射）** 强相关。
- **已排除「文件数据/页缓存被读坏」**（这是原来的首选假设，实测被否掉）：
  - 宿主侧用 `debugfs dump` 取出镜像里的 `java-21-openjdk/lib/modules`（142,510,592 B），md5 = `3d61e008965241fd1254b98b438184ee`；
  - 客体内 `md5sum` 同一文件 **3/3 次都返回同一个正确 md5**，`dd bs=1M/64k/4k` 也都完整读满 142510592 B；
  - 即读路径（read/page cache）本身正确，JVM 不是被坏数据喂死的。
- **但抓到了一个转瞬即逝的**用户态**崩溃**：另一次 diag 里，紧接着「`exit 7` / `kill -9 $$` / SIGSEGV」三个**进程退出**测试之后，
  `md5sum` 同一个大文件时 **自己 SIGSEGV（core dumped）**，而同一次会话里再跑就正常。说明是**偶发的内存损坏**，
  且**与进程/线程的创建-销毁活动相关**，不是文件内容问题。
- 退出状态编码本身是对的（`exit 7` → `$?=7`，`kill -9` → `137`），所以 `java` 的 255 是它自己真的 `exit(-1)`，
  不是内核把信号死亡报错。
- **也已排除「进程/线程退出清理写坏内存」**：按上面的思路做了复现器 —— 用 142MB jimage 的已知正确 md5
  （`3d61e008965241fd1254b98b438184ee`）当金丝雀，依次施加 `5× (java 后台 + kill -9)`、`5× java 正常退出`、
  `20× SIGSEGV`、`30× kill -9`，**每一阶段后校验都 OK**；内核日志里也没有 OOM/oom-killer、没有 `[SHFAULT]`/坏 pfn 之类的记录。
- **大文件 mmap 也验证通过**：用宿主 gcc 编了一个**无 libc、纯 syscall** 的静态 x86_64 小程序
  （`gcc -static -nostdlib -ffreestanding -fno-builtin -fno-pie`；同一二进制在宿主和客体都能跑），
  它对同一个 142MB jimage 同时做 `read()` 与 `mmap(MAP_PRIVATE)` 并各算一遍 FNV-1a：
  客体里 **8/8 次** `read fnv == mmap fnv == 宿主参考值 a14113a3dd22099e`（bytes=0x87e8a00=142510592）。
  → **读路径和 mmap 缺页路径都正确**，「文件映射/页缓存被读坏」这条彻底排除。
- **JVM 其实没坏（本轮最重要的修正）**：加 `-Xlog:class+load=info` 后，`java -version` 在**每一次**运行
  （含 `-Xshare:off`）都加载到 `java.lang.Shutdown` / `java.lang.Shutdown$Lock` 并打印完整正确的版本号 ——
  JVM **每次都跑完了**，只是**退出码**变成 255 而不是 0。所以「JVM 启动约 50% 失败」的说法不成立，
  JVM 本身可用（退出码不影响 Minecraft 启动）。
- 退出码 255 的来源：`kernel/proc/wait.c:112-118` 把 `exit_code` 映射成 wait status ——
  `code >= 0` → `(code & 0xFF) << 8`，`code < 0` → `(-code) & 0xFF`。所以 `$?=255` **不可能**来自 `exit_code = -1`
  （那会得到 `$?`=1/129），只能是 **`exit_code = 255`**（或信号死亡的 127 编码）—— 即 JVM launcher 真的返回了 255，
  与其 `DestroyJavaVM`/launcher 在「仍有线程没退干净」时返回 -1 的行为吻合。属于线程生命周期问题，不影响 JVM 计算。
- 本轮新增排除：进程/线程退出 churn（金丝雀 md5 四阶段全 OK）、OOM（内核日志无 oom-killer）、
  简单退出码编码（`exit 7`→7、`kill -9`→137），以及三条回收/映射路径（`mm_shared_file_fault` 的 pin、
  `swap_out_victim_pages` 的 PTE 替换、`MAP_PRIVATE` COW）。
- **并发线程互踩内存也已排除**：另一个无 libc 静态小程序（raw `clone(CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM)`，
  4 个子线程各有独立 mmap 栈和 4KB canary 缓冲，各自反复写/校验自己的 id 图案 30000 轮）在客体里
  **5/5 次全部 `RESULT: CLEAN`**（每个线程 mismatches=0），与宿主结果一致 → 线程之间不会互相写坏缓冲区。
- **退出路径探针（第三个无 libc 静态程序，`clone(SIGCHLD)` 每个变体 fork 一个子进程，父进程用 `wait4` 打印原始 status）**：
  客体 3/3 次结果完全一致，与宿主参考对比 ——
  | 变体 | 含义 | 宿主 | 客体 |
  |---|---|---|---|
  | v0 | `exit_group(0)` | code=0 | code=0 ✅ |
  | v1 | `exit_group(42)` | code=42 | code=42 ✅ |
  | v2 | 3 个自旋线程 + 主线程 `exit_group(0)`（**JVM 的退出模式**） | code=0 | code=0 ✅ |
  | v3 | 3 个自旋线程 + **非 leader 工作线程** `exit_group(42)` | code=42 | **status=0xb、sig=11(SIGSEGV)、exited=0** ❌ |
  | v4 | `exit_group(255)` | code=255 | code=255 ✅ |
  | v5 | `exit_group(-1)` | code=255 | **status=0x1、sig=1** ❌ |
- **v3 是本轮抓到的可复现内核 bug**：**从非 leader 线程调用 `exit_group` 时，进程被上报为「被 SIGSEGV 杀死」而不是带着退出码正常退出**。
  这很可能就是 `mpv` 那条「Lua 线程里 SIGSEGV」症状的来源（每个 Lua 脚本线程自己退出/收尾），值得优先修
  （`proc_exit_group()` 对 leader 的 `proc_force_exit()` 与 `proc_exit(self)` 组合，见 `kernel/proc/exit.c:562`）。
- 试过并**否掉**的一个候选：`proc_release_exiting_mm()`（`kernel/proc/exit.c:161`）在**每个**线程退出时都会
  `arch_switch_addr_space_token(kernel_as)` + `mm_context_leave(t->mm, cpu)`，看起来像「兄弟线程还在跑就把本 CPU
  从共享 mm 上摘掉」。按「只有 mm 最后一个引用才允许摘」加了 `refcount_read(&mm->refcount) == 1` 守卫、重新编译内核并复测：
  **v3 仍是 `sig=11`，java 仍是 `255 0 255 0 255 0`**，桌面无回归 → 该假设不成立，改动已 `git checkout` 撤回。
  说明 leader 是**真的**发生了缺页（`signal.c:768` 用 `-signal_wait_status(SIGSEGV)` 收尾），而不是 active_cpus 记账问题；
  下一步应从「强制一个正在用户态自旋的线程退出」这条路径查（`proc_force_exit` → `exit_pending` → 调度器/trap 边界消费），
  以及 leader 缺页时它的 mm 到底处于什么状态。
- **v2 与 v3 的差别把范围缩得更小**：v2（**leader 自己**调 `exit_group`，其他线程还活着）正常；v3（**非 leader** 调 `exit_group`，
  于是 leader 被 `proc_force_exit()` 强制退出）报 SIGSEGV。也就是说触发点是「**从非 leader 线程强制退出 group leader**」，
  而不是「有活线程时退出 group」本身。
- 想再区分「被强制的线程处于用户态自旋 vs 阻塞在系统调用」而加的 v6/v7 变体**在宿主上也是 sig=11**，说明**是我探针自己写错了**
  （宿主不该 SIGSEGV），这两个变体作废、该区分仍未被测到；不影响 v3 的有效性（v3 宿主 code=42、客体 sig=11）。
- **更正：v3 本身也是我探针的 bug，不是内核 bug（本条作废上面 v3/v2 的结论）**。把内核日志里的 `SIGSEGV: sepc=0x40135b stval=0x60030008, sp=0x60030000`
  反汇编后发现：崩溃点是 `worker_group` 线程**启动后的第一条指令** `movq $0x0, 0x8(%rsp)`（初始化循环计数器），而 `sp=0x60030000` 恰好是
  该线程 `mmap` 出来的栈区的**上界**，于是 `[rsp+8]` 落在映射区**之外**。也就是说探针把 `child_stack` 设成了 region 的**末尾**（exclusive），
  `call fn()` 把返回地址压到 `rsp-8` 后，callee 的第一个局部变量又写回 `rsp+8`，正好越界。Linux 上"碰巧"没事，是因为相邻的匿名 `mmap`
  往往首尾相接、越界那几字节落进了下一个映射；A20OS 不这么排布 → **正确地** fault。给 `child_stack` 留了 4KB 余量后，
  客体里 `v3` **5/5 次都是 `code=42/sig0`，与宿主一致**。所以：`exit_group`（含非 leader 触发、强制退出自旋中的 leader）在 A20OS 上是
  **正确的**；此前归因于它的分析撤回。这个教训值得记下：**手写 clone 的 child_stack 不能落在映射末尾**，且"宿主能跑"不等于"探针没问题"。
- **结论：内核的核心线程/内存/退出机制经逐一探测均正确**。同一套「宿主+客体对跑」的无 libc 静态探针证明了：文件 `read()` 与 `mmap()`
  数据完全一致（142MB jimage FNV 匹配 8/8）、并发线程不互踩各自缓冲（5/5）、`exit_group` 各形态（含非 leader 触发）退出码正确（5/5）、
  每线程 FS base/TLS 独立不被共享（每线程 `arch_prctl(ARCH_SET_FS)` 指向自己的块再循环读 `%fs:0`，4/4 次全 0 错配）。
  因此 `mpv` 偶发崩溃与 `java` 退出码 255 **不是**由这些机制引起的；剩下可查的方向是 futex/信号等更细的语义，
  或它俩本就是程序层行为。对「视频可用」这个目标而言不受影响（ffplay 已验证可用，见下文 ffplay 条目）。
- **v5 是编码分歧**：A20OS 把负的 `exit_code` 当作信号死亡编码（`wait.c:112-118` 的 `code < 0` 分支 → `(-code) & 0xFF`），
  而 Linux 对 `exit_group(-1)` 报的是正常退出 code=255。属于 ABI 语义差异，单独记录。
- v2 与宿主一致说明「主线程带活线程 `exit_group`」这条 JVM 路径本身没问题；结合前面 `java.lang.Shutdown` 的证据，
  java 的 255 更可能是某个退出码/编码产物（例如内部以负值或 255 收尾），**不影响 JVM 运行**。
- **剩余真正待查的是「多线程进程的匿名内存偶发被写坏」**：唯一还站得住的症状是 `mpv` 约 1/10 崩溃
  （LuaJIT GC 链表头变 NULL），单线程 LuaJIT 完全正常。方向应查**线程生命周期**（线程退出/reap、内核栈、
  per-thread trap 帧归属），而不是继续在文件 I/O 上找。建议下一步给内核线程退出路径加 trace/校验
  （`proc_exit`/`proc_force_exit`/`exit_pending` 与 `pending_exit_code` 的一致性），并用 `mpv --vo=null` ×N 复现。

- **2026-09 补充：同一个「链表头被写坏成 NULL」的签名，在一个完全无关的程序里又出现了一次。**
  Minecraft 的死因链最后落在 `ld-musl-x86_64.so.1+0x4602b`，反汇编是
  `mov -0x10(%rdi),%rax; lea -0x10(%rdi),%rcx; cmp %rcx,0x10(%rax)` —— 一条**双向链表自指针
  一致性检查**；内核日志给出的 `stval=0x10` 说明当时 **`rax == NULL`**
  （另一轮 `stval=0x14c834000` 则是同一处 `rax` 变成野值）。这与本条目上面 mpv 的
  `libluajit+0x53320` 读 `*(G+0x130)` 得到 NULL **是同一个形状**：
  **一张本该有效的链表头被写成了 NULL/野值**。
  ⇒ 两个互不相关的程序（LuaJIT、musl 的 ld.so）在同一内核上出现同一签名，让「多线程进程的
  匿名内存偶发被写坏」这条结论**明显更站得住**，而不是各自程序的 bug。

  同时**排除了「展开器 / DSO 查找逻辑本身坏了」**：用宿主 g++ 编的无头文件 freestanding C++
  最小用例（`-nostdlib`；`__cxa_*`/`_Unwind_*`/`_ZTIi` 由客体 libstdc++/libgcc_s 在加载时解析），
  由已有的 `LD_PRELOAD` 垫片在第一次 `dlopen` 时驱动，覆盖 5 种情形 —— 主线程捕获、主线程
  重抛+析构、主线程未捕获、非主线程捕获、非主线程未捕获 —— **全部行为正确**
  （42 / 14 / terminate+abort / 5 / terminate+abort），**一次都没有出现 `ld-musl+0x4602b`**。
  细节与日志见 `docs/distro/minecraft.md`。

- **现成的检测器有一个，但它是关着的（这是最值得先动的一步）**。`kernel/mm/slab.c` 里：
  - `BIG_CANARY 0xCAFEBABE`（第 58 行）**只覆盖 big-alloc 块**：块尾哨兵在 279 行写入，
    `big_alloc_canary_ok()`（61-66 行）校验，而且**只在 `kfree` 时校验一次**（377-382 行；命中就打
    `[SLAB BUG] kfree(...): big-alloc canary clobbered` 并 `panic`）。
    ⇒ 活着的对象被写坏要等它被释放才发现；**一直不释放就永远发现不了**。
  - `slab_validate_sp()`（230-257 行）是一个**已经写好的 slab 页完整性校验器**，检查的正是我们
    怀疑的这一类损坏：free_list 节点越界/未对齐、free_list 成环或溢出（`free_count > total`）、
    以及 `in_use + free_count != total` 的计数不符；命中时还会把该页前 16 个 64 位字 dump 出来。
    **但它被标了 `__attribute__((unused))`，两处调用点都被注释掉了** —— 321 行（`kmalloc`，
    `"kmalloc-pre"`）与 468 行（`kfree`，`"kfree-pre"`）：`// slab_validate_sp(sp, "kmalloc-pre", obj_size);`

- **对上面那个检测器做了一遍代码审查（结论：可以放心打开）**：
  - **它遍历链表的方式是自保护的**：循环体先把当前节点 `p` 校验为「在本页内且按 `obj_size` 对齐」，
    然后才在循环步进里读 `*(void **)p` 取下一个节点 ⇒ 即使链表已经被写坏，它也**不会自己 fault**，
    而是带着诊断 `panic`。这正是探针该有的行为。
  - **类型是安全的**：`sp->total`/`sp->in_use` 是 `uint16_t`、`free_count` 是 `int`，
    `(int)(in_use + free_count) != total` 的整型提升没有溢出/回绕风险；`free_count > total`
    的成环判定也正确（对象数很小）。
  - **锁上下文两处都是对的**（这点很关键，因为文件自己在 421-431 行强调「所有参与
    free-list/bitmap 状态机的检查都必须在 cache 锁内」）：`kmalloc` 的 321 行在
    `spin_lock_irqsave(&c->lock)`（288 行）之后，且成功路径上没有解锁；`kfree` 的 468 行同样在
    419 行加锁之后 ⇒ 两处都满足前提。
  - **它要抓的形状正是文件自己描述过的那个**：421-431 行的注释写明「两个 CPU 同时释放同一对象会
    都看到 `alloc_bits` 已置位、于是把对象插入两次（通常形成自环），而这种损坏只会在之后的
    `kmalloc()` 里被发现」—— 这恰好对应 `slab_validate_sp` 的 `free_count > total`
    → `panic("slab_validate_sp: free_list cycle")`。
  - **代价**：每次调用 O(空闲对象数)，在热路径上是可感知的开销 ⇒ 适合当诊断用，不宜长期默认开。

  ⇒ **建议的第一步（很便宜，且已确认前提成立）**：把 321 与 468 两处调用打开（并去掉 `unused`），
  必要时在 `kmalloc` 取到对象**之后**再校验一次；然后按本条目的复现方式（`mpv --vo=null` ×N，或多线程
  + 大量 `dlopen` 的压力负载）跑。它是为这一类损坏写好的现成探针，一旦命中就直接给出 `where` 和
  页面 dump，比继续做上游推理快得多。抓到人（或连续多轮全干净）之后，再决定要不要长期保留。

- **【已实测】把上面那个检测器打开跑了压力负载：它**没有**命中，但同一轮把目标 bug 复现了出来**
  （改动只用于诊断，跑完已经 `git checkout -- kernel/mm/slab.c` 还原，内核代码保持干净。）

  改动就是前面那三步：去掉 `slab_validate_sp` 的 `__attribute__((unused))`、打开 321（`kmalloc-pre`）
  与 468（`kfree-pre`）两处调用；`make ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both dev-build` 通过。
  然后注入压力负载：桌面起来后跑 **40× `java -version` + 40× `mpv --vo=null`**（都是短命、多线程的进程，
  专门制造线程/进程的创建-销毁 churn）。

  结果两边都很有信息量：

  - **✅ 负载确实复现了目标 bug，签名与本文档上面记的逐字节一致**：
    ```
    SIGSEGV: pid=631 code=13 sepc=0x638a6320 stval=0x8 abi=0
    FATAL:   pid=631 signal=11 abi=0 pc=0x638a6320 sp=0x77a3eb70 comm=lua/osc path=/extra/usr/bin/mpv
    vma_file=/extra/usr/lib/libluajit-5.1.so.2
    ```
    `comm=lua/osc`、`path=/usr/bin/mpv`、`libluajit-5.1.so.2`、`sepc=0x638a6320`、`stval=0x8` ——
    与本文档第 69 行那条记录**完全对上**；40 次 mpv 里**第 1 次就崩了**（`rc=139`）。
    ⇒ 这套负载可以直接当**可用的复现器**，比原来「约 1/10」的描述好用得多。
    失败信息 `stval=0x8`、`pte=0x0 value=0x0`（该地址连页表项都没有）⇒ 又是一次
    **「读一个近 NULL 指针的小偏移」**，与 `ld-musl+0x4602b` 那次的 `stval=0x10` **是同一个形状**。

  - **❌ slab 检测器全程没有命中**（80 次迭代，无 `[SLAB BUG]`、无 `slab_validate_sp` panic）。
    ⇒ **这类损坏不是内核 slab 的 free-list 损坏**。这和两个已知实例都是**用户态**结构这一点吻合：
    LuaJIT 的 `G`（`*(G+0x130)`）与 musl ld.so 的那张链表头，都在**进程自己的地址空间**里，
    内核的 slab 校验器根本看不到它们。
    ⇒ 检测方向应从「内核 slab」转到 **「多线程进程的匿名内存被写坏」**——正好回到本文档第 152 行
    的结论，只是现在多了两条硬证据：**slab 侧已排除**，以及**两个 `NULL+小偏移` 的实例**。

- **【已实测】用户态匿名内存金丝雀：32 MiB 保护带在 40 个 mpv 进程里全程完好 ⇒ 不是「野写」
  也不是「整页被清零」**

  思路：既然两个实例（LuaJIT 的 `G`、musl ld.so 的链表头）都在**进程自己的地址空间**里，
  就把金丝雀直接放进**出事的那个进程**，而不是另写一个程序 —— 给已有的 `LD_PRELOAD` 垫片加一段：
  `mmap` 32 MiB（8192 页）匿名内存，每个 64 位字填成 `0xCA9E000000000000 + i`（每个字唯一），
  再起一个线程每 100 ms 全量校验，发现不等于期望值就打印 `CANARY_MISMATCH`。
  然后用 `LD_PRELOAD=/usr/lib/libthrowtrace.so` 装进 mpv，跑 40 次 `mpv --vo=null`（沿用上面已复现的负载）。

  ```
  t=58515 tid=155 CANARY_START a=0x77840000 b=0x2000000     # 40/40 次都装上了
  t=58518 tid=155 CANARY_THREAD a=0x0                       # 校验线程 40/40 起成功
  ...(40 次)
  SIGSEGV: pid=158 code=13 sepc=0x638ab320 stval=0x8 abi=0  # 目标 bug 照旧复现
  FATAL:   pid=158 ... comm=lua/osc path=/extra/usr/bin/mpv
  ```

  - **`CANARY_MISMATCH` = 0，`CANARY_BAD_PASS` = 0**：32 MiB（8192 页）的保护带在**每一个** mpv
    进程里、**包括崩掉的那一个**，从安装到进程结束**一个字节都没被改过**。
  - ⇒ **排除「野写 / 越界写碰巧落进匿名内存」**：若是大范围乱写，32 MiB 的靶子几乎必然被打中；
    一次都没中 ⇒ **不是随机野写**。
  - ⇒ 同时**排除「整页被清零 / 误回收」**：8192 页中任何一页被清零都会被逐字校验抓到，而 0 次。
  - ⇒ 合起来：损坏的粒度是**亚页级、针对特定结构**的，且结果常是**把某个指针字段写成 0/NULL**
    （正是两个实例的形状）。这与两种机制吻合：**use-after-free / 过早复用**（结构被释放后又被
    别人分配并清零），或**基于错误基址的存储**（例如 TLS/FS base 错位，使写入落在一个
    「确定但错误」的地址上 —— 这种写入恰好不会碰到我们那块保护带）。

  两条 harness 经验：
  1. **`mpv` 完全不调用 `dlopen`**（它的库都是链接期依赖）⇒ 第一版把金丝雀挂在 `dlopen` 拦截器上，
     结果 `CANARY_START=0`、什么都没测到（假阴性！）。改挂到 `pthread_create` 拦截器
     （线程创建本就是这类损坏的相关活动）后 40/40 都装上了。
  2. 金丝雀的 `mmap(NULL)` 落点每次相同（`0x77840000`）—— 做地址相关性对照时可用这一点。

- **【已实测】加上「free 毒化」再跑同一负载：毒化确实生效，但崩溃签名一点没变、毒化值一次没出现**

  做法：在垫片里拦截 `free`，释放前把整块 payload 填成 `0xDEADB33FCAFEF00D`，并周期性打印计数。
  用同一套 mpv churn 负载（40 次 `mpv --vo=null`）。

  ```
  t=58487 tid=148 #1  POISON_MARKS a=0x1    b=0x20      # 拦截生效：第 1 次 free 毒化 0x20 字节
  t=58822 tid=162 #24 POISON_MARKS a=0x1000 b=0x76c78   # 4096 次 free、486 KiB
  ...共 235 条 ⇒ free 拦截确实在工作
  SIGSEGV: pid=151 code=13 sepc=0x638ac320 stval=0x8    # 与基线逐字一致
  [FAULT-VA] stval=0x8
  FATAL: pid=151 ... comm=lua/osc path=/extra/usr/bin/mpv
  毒化值在整份日志里出现次数：0
  ```

  - **毒化生效是确定的**（235 条计数、第一条 `a=0x1 b=0x20`）⇒ 这不是「探针没装上」的假阴性
    （第一版就是因为日志触发条件写成 `(g_frees & 0xFFFF)==0`、只在恰好 65536 次时打印，白跑了一轮）。
  - **签名没变、毒化值零次出现** ⇒ **排除「读一个仍然带毒化值的已释放块」**。
  - **但有一个变体仍然成立，必须说清**：若是 UAF，而那块内存在我毒化之后又被**合法的新主人
    重新分配并清零**，过期读者看到的**正是 0/NULL** —— 与观测到的 `stval=0x8` **完全一致**。
    所以 **「use-after-free + 复用后被清零」并未被排除**，被排除的只是「读到还带毒化值的空闲块」。
  - ⇒ 真正的区分办法是**隔离式分配器（quarantine）**：`free` 后**不把块还给分配器**（毒化后扣住，
    设上限），这样过期引用**永远读不到被清零的新内容**，只会读到毒化值；同时可周期扫描扣住的块，
    **直接抓出「往已释放内存里写」**（比「读」更硬的证据，而且不必等崩溃）。
    这是下一步最该做的实验。

  这一轮还多抓到一个受害者：**`tumblerd`（缩略图服务）也 SIGSEGV**
  （`ADE/ALE: pid=123 sepc=0x432c6031 stval=0x8136a00e code=1`），与本文档前面记过的 tumblerd 野指针
  是同一件事 —— 再次说明这个损坏**跨进程、跨程序**。

- **【已实测，但本轮无结论】隔离式分配器（quarantine）：扫描线程起来了 40/40，可每次扫描时隔离区都是空的**

  做法：`free` 里先毒化、再**不把块还给分配器**（扣进隔离区，上限 65536 块/进程），另起线程周期性扫描
  隔离区；任何块不再等于毒化值就报 `FREE_WRITE`（= **往已释放内存里写**，比「读」更硬的证据）。

  ```
  QUAR_THREAD = 40/40                    # 扫描线程每个 mpv 进程都起成功
  QUAR_SCAN   = 40，全部 a=0x1 b=0x0      # ← 每进程只跑 1 轮，且那一轮 g_qn == 0（隔离区空）
  FREE_WRITE  = 0
  SIGSEGV: pid=163 code=13 sepc=0x6396c320 stval=0x8    # 签名仍与基线一致
  毒化值出现次数：0
  ```

  - 线程起来了，但**每进程只扫描 1 轮**（`a=0x1`），而那一轮 `g_qn == 0`
    ⇒ **唯一的扫描时刻隔离区是空的**，等于什么都没检查 ⇒ 本轮**既不能证明也不能排除**
    「往已释放内存里写」。
  - 原因已看清：扫描线程在**第一次 `free` 时**才启动，启动后第 1 轮立即执行（此时还没扣下任何块），
    然后 `nanosleep` 200 ms —— 而 mpv 在第一次 `free` 之后活不到 200 ms 就退出了
    ⇒ **永远只有第 1 轮**。这是 harness 的时序问题，不是假设被否定。
  - **修法（下一步）**：让宿主进程活得远长于 200 ms —— 最简单是把负载换成 `mpv --vo=null --idle`
    （常驻，验证完再 kill），或者把金丝雀/隔离区放进**长期存活的 java 进程**（java 也是已确认的受害者：
    MC 那次崩溃就在 java 里）。这样隔离区能积累成千上万块、扫描能跑几百轮，才有机会抓到
    「往已释放内存里写」。
  - 顺带**又踩了一次「编译失败却注入了旧 `.so`」**：新代码里我写了 `u32`，而垫片只 typedef 了 `u64`，
    gcc 报 `unknown type name 'u32'`；但命令是 `gcc ... && echo` 串联的，报错后流程没停，
    于是**把上一版的 `.so` 注进了镜像**（现象：`QUAR_*` 标签一个都没有、签名跟上一轮逐字相同）。
    已经在流程里加了**硬失败保护**（`if ! gcc ...; then exit 1; fi`）：编译不过就绝不注入。
    教训：**探针流水线必须让编译失败中止整条链**，否则会拿旧产物跑出一份看似「阴性」的结果。

- **【已实测，有结论】隔离区（quarantine）跑通：7910 个已释放块被扣住并扫描 ~150 轮，
  `FREE_WRITE` 为 0、签名仍是 `stval=0x8` ⇒ **use-after-free 被排除**（读与写两种形式都排除）**

  把负载换成**长期存活**的宿主（`mpv --vo=null --idle`，每个活 25 秒、共 6 轮），这样扫描线程能跑够
  轮数、隔离区也能积累。注入前后都加了硬校验（`QE0`/`--idle`/`LD_PRELOAD` 三者缺一即 `exit 1`），
  所以这轮数据可信：

  ```
  QE0 → QE1 → mpv m=1..6 killed → QE2                 # 6 个长期宿主，各 25 秒
  QUAR_SCAN = 621 轮（全轮）
  t=217509 tid=219 #147 QUAR_SCAN a=0x78   b=0x1ee6    # 隔离区 0x1ee6 = 7910 块，且持续被扫描
  ...
  t=218315 tid=219 #151 QUAR_SCAN a=0x7c   b=0x1ee6
  FREE_WRITE = 0
  SIGSEGV: pid=152 code=13 sepc=0x6396c320 stval=0x8    # 与基线逐字一致
  毒化值出现次数：0
  ```

  - 隔离区里 **7910 个已释放块被扣住并毒化**、被扫描 **~150 轮**（全轮 621 次扫描）
    ⇒ 覆盖面足够，不是「没测到」。
  - **`FREE_WRITE` = 0** ⇒ **没有「往已释放内存里写」**。
  - **签名仍是 `stval=0x8`、毒化值一次未出现** ⇒ 那个 NULL **不是**从已释放（且**永不复用**）的块里
    读出来的 —— 若是，隔离区会让它读到毒化值，而不是 0。
  - ⇒ **use-after-free 被排除**（读、写两种形式）。这同时关掉了前面「UAF + 复用后被清零」那个
    仍然成立的变体，因为隔离区**根本不复用**。

  ⇒ 到这里的排除清单已经相当长：**内核 slab free-list、pbuf 双重释放、通用展开/DSO 查找逻辑、
  线程创建失败、音频后端、dlopen 卡住、匿名内存野写、整页清零/误回收、use-after-free**。
  剩下的候选集中在两处：
  1. **基于错误基址的存储**（例如 TLS/FS base 错位 ⇒ 写入落在一个「确定但错误」的地址，
     且写进去的多半是 0 或小值 —— 正好不会碰到大范围金丝雀，也不是复用已释放块）；
  2. **内核在某条路径上「返回 0 / 交出一张零页」**（在页级或 syscall 返回值层面，而不是 malloc 复用）。
  下一批实验应针对这两条。

- **【代码级发现，候选机制】`clear_child_tid` 的清理是「绕过权限/COW 的裸物理写」，且用户指针毫无校验**

  位置：`kernel/proc/exit.c` 的 `proc_clear_child_tid_direct()`（`proc_exit` 第 377 行调用）：

  ```c
  static void proc_clear_child_tid_direct(task_t *t) {
      if (!t->clear_child_tid) return;
      int *ctid = t->clear_child_tid;
      t->clear_child_tid = NULL;
      if (!t->pgdir) return;
      paddr_t pa = pt_translate(t->pgdir, (vaddr_t)(uintptr_t)ctid);   // 只翻译
      if (!pa) return;
      pfn_t pfn = phys_to_pfn(pa);
      if (!pfn_valid(pfn)) return;
      int *kv = (int *)((char *)pfn_to_virt(pfn) + ((uintptr_t)ctid & (PAGE_SIZE-1)));
      *kv = 0;                                                        // 直接写物理帧
  }
  ```

  与 Linux 语义有两处**实质差异**：

  1. **绕过了所有权限 / COW 检查**。Linux 在这里用带访问检查的 `put_user(0, tidptr)`
     （不可写 / 未映射会安全失败）；这里是「翻译出物理帧 → 经内核线性映射直接写」，
     **不检查该页此刻是否可写、是否 COW 共享、是否只读文件页**。
     - 若该页正与别的任务 **COW 共享**，这一写会把**对方那份**也改掉；
     - 若指向**只读文件页**，还会改到**页缓存**，影响所有映射它的进程。
  2. **用户指针完全没有范围 / 有效性校验**。`sys_set_tid_address()`
     （`kernel/abi/linux/sys_proc.c:135-139`）只是 `t->clear_child_tid = tidptr;`
     —— 这与 Linux 一致（Linux 也不校验指针），**差别在于 Linux 的写是受检查的、这里的是裸物理写**。

  **为什么它值得优先验证**（与前面所有排除项都对得上）：
  - 写进去的是 **0** ⇒ 正好是「某个指针字段变成 NULL」那个签名；
  - 触发点是**任务退出**（`proc_exit`）⇒ 与「线程/进程创建-销毁 churn」这个已知相关性**完全吻合**；
  - 是**亚页级、精确地址**的 4 字节写 ⇒ 会被 32 MiB 金丝雀漏掉（而金丝雀已证明不是野写），
    也不是整页清零、更不是 use-after-free（这三条都已实验排除）；
  - 若落在 COW 共享帧或页缓存上，就是**跨进程**污染 ⇒ 与「tumblerd / mpv / java 都中招」吻合。

  **验证方案（两条任一即可给结论）**：
  （a）**先只加探测、不改行为**：在 `*kv = 0;` 之前判断目标是否「本任务私有且可写」（或查该物理帧的
      引用计数 / 是否 COW 共享），不是就打一条 `[CTID]` 日志；用已经可复现的 mpv 负载
      （40 次里约 1 次崩）跑几轮，看这条日志是否出现。
  （b）**直接改成受检查的用户写**（等价于 Linux 的 `put_user(0, ctid)`：检查映射存在且可写，
      必要时走 COW 破坏流程），再跑同一负载，看 mpv / tumblerd 的崩溃是否消失
      —— 因果性最强的检验。

  **保留意见（不要过度断言）**：这条能解释**用户态**那个「指针字段变 NULL」的签名，但解释不了内核侧
  「pbuf 里出现**用户态地址**」那一幕（那需要「写入一个用户态指针」，而这里是写 0）。两条线可能仍需
  分开收尾；本条应作为**候选机制**去验证，而不是当作已定的根因。

- **【根因已定位，并有确定性最小复现】就是 `clear_child_tid` 那条「裸物理写」缺了 COW 破坏
  —— 跨进程内存损坏的原因找到了**

  用一个**纯用户态、最小、确定性**的复现器证实了上面那条候选机制。复现器逻辑：

  1. 父进程把全局变量写成 `0x12345678`，然后 `fork()`；
  2. 子进程**不写**它（于是该页与父进程 **COW 共享**），把 `set_tid_address()` 指向它，然后 `exit()`；
  3. 父进程 `wait4()` 后检查自己那份还是不是 `0x12345678`。

  正确实现（Linux 用受检查的 `put_user(0, tidptr)`）会在写之前破坏 COW、给子进程一张**私有副本**，
  父进程那份保持原值；而 A20OS 是「翻译出物理帧 → 经 `pfn_to_virt` 直接写该帧」，
  于是**父子共用的那一帧被写成 0** ⇒ **父进程的变量被改成 0**。

  同一个二进制在宿主与客体对跑（这是本项目一贯的「宿主参照」方法）：

  ```
  宿主（参照，Linux）： test 12345678 clean     control 0badf00d clean    # 5/5 轮全 clean
  客体（A20OS）：      test 00000000 POLLUTED  control 0badf00d clean    # 20/20 轮全 POLLUTED
  ```

  - `test` 轮（目标页与父进程 COW 共享）：客体 **20/20 全部被写成 0**，宿主 5/5 完好。
  - `control` 轮（子进程**先写**该页、真正破坏 COW 变成私有）：客体**也是 clean** ✓
    ⇒ 这证明失效点**精确地**是「**没有做 COW 破坏**」这一环，而不是别的东西。
  - 复现率 **20/20（确定性）**、宿主全对 ⇒ **这是 A20OS 的 bug，不是探针问题**。
  - 复现器：`/tmp/opencode/ctidtest.c`，构建
    `gcc -static -nostdlib -ffreestanding -fno-builtin -no-pie -o ctidtest ctidtest.c`
    （静态、无 libc，宿主与客体都能跑）；客体里放在 `/usr/bin/ctidtest`。

  **它解释此前全部观测，也与所有排除项相容**：
  - 写进去的是 **0** ⇒ 正是「某个指针字段变成 NULL」那个签名；
  - 触发点是**任务退出**（`proc_exit`）⇒ 与「线程/进程创建-销毁 churn」相关性吻合；
  - COW 共享的那一帧**同时属于另一个任务** ⇒ **跨进程污染**（mpv / tumblerd / java 都中招）；
  - 精确地址的 **4 字节**写 ⇒ 亚页级，**不会被 32 MiB 金丝雀打到**（金丝雀已证明不是野写）；
  - 既不是野写、也不是整页清零、更不是 use-after-free（这三条均已实验排除）；
  - 与内核 slab、pbuf 双重释放、展开逻辑、线程创建失败均无关（均已排除）。

  ⇒ **本条长期悬而未决的「多线程进程的匿名内存偶发被写坏」到此定位到具体代码与具体语义缺陷**：
  `kernel/proc/exit.c` 的 `proc_clear_child_tid_direct()` 用裸物理帧写替代了受检查的用户写，
  缺少 COW 破坏与权限检查。

  **修法**：把这条裸写换成**受检查的用户写**，等价于 Linux 的 `put_user(0, ctid)` ——
  若目标页是 COW（或只读），先按正常流程**破坏 COW**、拿到本任务私有副本，再写 0；
  不可写/未映射时安全失败（像 Linux 一样直接返回）。

  **修复与验证（均为已实测）**：
  - **修法**：把 `proc_clear_child_tid_direct()` 里的裸物理写换成**受检查的用户写**
    `copy_to_user(ctid, &zero, sizeof(zero))`。核心里这条路径本来就会做该做的事 ——
    `user_resolve_leaf(..., write=1, ...)`（`kernel/mm/mm.c:585-597`）在叶子不存在时走
    `handle_demand_fault()`、在存在但**不可写**时调用 `handle_cow_fault()` **破坏 COW**，
    再重读 PTE，最后对未映射/不可写的地址安全返回 `-EFAULT` —— 即 Linux `put_user(0, tidptr)` 的语义。
    出问题的那个函数绕过了它，自己 `pt_translate + pfn_to_virt` 直接写帧 → 跳过了 COW 破坏。
  - **验证（同一个复现器、同一个镜像，只换内核）**：
    ```
    修复前：test 00000000 POLLUTED   control 0badf00d clean   # 20/20 与 20/20
    修复后：test 12345678 clean      control 0badf00d clean   # 20/20 与 20/20  ← 与宿主逐项一致
    ```
    修复后 `test` 轮 20/20 的污染**全部消失**，且与宿主（Linux）结果一致。
  - **为什么它是偶发的**：只有当任务的 `clear_child_tid` 所指的页**此刻与另一个任务 COW 共享**时才发作，
    所以表现为「约 1/10」；一旦发作就是**把另一个任务（通常是 fork 出来的父子进程）的 4 字节字段清零**，
    于是症状永远是「某个指针字段变成 NULL」。这与 mpv / tumblerd / JVM 那几条症状完全对得上。

- **【重要更正】cleartid 这个缺陷是真的、也真的修好了，但它**不是** mpv/tumblerd 那几条崩溃的原因**

  修完之后，用**同一套会复现症状的负载**（这次加大到 60× `java -version` + 120× `mpv --vo=null`）
  在**修复后的内核**上再跑一遍，结果**症状照旧出现**：

  ```
  VF java done=60   VF mpv done=120                      # 负载确实跑完了
  SIGSEGV: pid=869 code=13 sepc=0x638a6320 stval=0x8     # 与修复前**同样的**签名
  FATAL:   pid=869 ... comm=lua/osc path=/extra/usr/bin/mpv
  ADE/ALE: pid=123 sepc=0x45bb6031 stval=0x8136a00e code=1
  FATAL:   pid=123 ... comm=tumblerd                     # tumblerd 也照旧崩
  ```

  ⇒ **必须更正上面那句「完全对得上」**：
  - `clear_child_tid` 的缺陷**本身是真的**、修复也是对的（`ctidtest`：修复前 20/20 污染、
    修复后 20/20 干净，宿主对照 5/5 全对）—— 它确实是一个**独立的、确定性的**跨进程内存损坏 bug，
    **修复应当保留**；
  - 但它**不是** mpv / LuaJIT / tumblerd 这些偶发崩溃的原因：修复后这些症状**依然复现**，
    且签名逐字相同（`comm=lua/osc`、`sepc=0x638a6320`、`stval=0x8`）。
    此前仅凭「形状一致」就把它认定为那些症状的根因，属于**过度推断，在此撤回**；
  - 同理，MC 那次 `ld-musl+0x4602b` 崩溃**没有**在修复后复测过，所以对它同样**不能**下这个结论；
  - ⇒「多线程进程的匿名内存偶发被写坏」这条**仍未解决**（只是排除项又多了一条：
    cleartid 的 COW 缺陷已被证明是另一回事）。**mpv 崩溃的根因仍然未知。**

- **【同类缺陷：审计发现第二处】`futex` 的 PI 路径也在「自己翻译地址、裸写物理帧」，而且写的也是 0**

  在排查 mpv 那条崩溃时，把「自己 `pt_translate` 出物理帧再直接写」这个**已被证明有害**的写法在核心里
  过了一遍，发现**第二处同类代码**：

  - `kernel/ipc/futex.c` 的 `futex_user_word_map()`（541-564 行）`pt_translate` → `pfn_to_virt` 后
    **返回一个指向内核线性映射的 `volatile int *word`**，调用方拿它做**原子写**：
    ```c
    volatile int *word = NULL;
    futex_user_word_map(t, uaddr, &word, &pkey);      // 581 / 654 行
    ...
    __atomic_store_n(word, zero, __ATOMIC_RELEASE);   // 668 行：同样是裸帧写，写的也是 0
    ```
    ⇒ 与 cleartid 那条**完全同类**：若目标页此刻与别的任务 **COW 共享**，这一写会把**对方那份**
    也改成 0。
  - **已核对为「正确」的一处**：`exit_robust_list()`（680-713 行）用的是 `copy_from_user()` /
    `copy_to_user()` ✓ —— 说明核心里**正确写法是有的**，出问题的两处（cleartid 与这里）都是**自己手写**。
  - 也顺带澄清语义边界：**对 MAP_SHARED 页裸写帧是对的**（大家都映射同一帧，本就该一起改），
    错的只是 **COW-private** 的情况；`user_resolve_leaf()` 两种情形都能正确处理 ✓。

  **为什么这次记录为待修、而不当场改**：
  - **触发面很窄**：这条只在 **PI（优先级继承）/ robust futex** 路径上写；而 mpv / LuaJIT /
    tumblerd / JVM 用的是**普通 futex**（`FUTEX_WAIT/WAKE`，走的是只读的 `futex_user_load_locked()`）
    ⇒ 它**大概率解释不了**那几条偶发崩溃（这也是为什么先不动它）。
  - **修复有死锁风险**：`futex_user_word_map()` 的调用点在 **`mm->lock` 之内**（该函数注释明确写了
    “Caller must hold mm->lock”），而正确的 COW 破坏要调 `handle_cow_fault()`
    （声明在 `kernel/include/mm/fault.h:11`）；若它内部也取 `mm->lock`，原地调用就会**自死锁**。
    因此修法必须把 COW 破坏挪到取 `mm->lock` **之前**（或另设无锁路径）；
    也不能照搬 cleartid 那招——这里要的是**原子写**，而 `copy_to_user()` 是 memcpy，不适用。
  ⇒ 记为**已定位、待修的同类缺陷**，修法要点如上。

- **【已实测，阴性】COW 破坏本身是保数据的：宿主/客体对跑 20/20 都干净 ⇒ 这条假设排除**

  **动机（此前测试的一个盲点）**：32 MiB 金丝雀**只读不写**，所以它**从未触发过 COW 破坏** ——
  也就是说「**COW 破坏是否正确**」这一路此前**完全没测过**。而 mpv 会 `fork()`（ytdl/osc 脚本），
  fork 之后**父进程**再写那些继承来的页就必须触发 COW 破坏；若破坏时拷错（或拷成零页），
  父进程的页就会变成 0 ⇒ 指针字段变 NULL，与签名吻合。于是补了这个确定性最小复现器：

  - 父进程把整页填成 `i ^ 0x5A` 的图案 → `fork()` → 子进程**睡 300ms 保持存活**（让页面仍与父
    COW 共享）→ 父进程只写 **1 个字节**（必然触发 COW 破坏）→ 校验**其余 4095 字节**是否原样；
  - `control` 轮：同样填+写但**不 fork**（无 COW）⇒ 任何实现都应干净，用于证明探针本身没问题。

  实测：
  ```
  宿主（参照）：cow 00000000 clean   control 00000000 clean    # 5/5 全干净
  客体（A20OS）：cow 00000000 clean   control 00000000 clean    # 20/20 全干净
  ```
  ⇒ **「COW 破坏会损坏数据」这条假设被排除**（`bad=0`，与宿主逐项一致）。
  （复现器：`/tmp/opencode/cowtest.c`，同样是静态、无 libc、宿主客体都能跑的 freestanding 程序。）

- **【已实测，阳性】PI futex 那条同类缺陷被证实：`pi 00000001 POLLUTED` 20/20（宿主 5/5 干净）**

  用与 cleartid 完全相同的「确定性最小复现 + 宿主对照」方法，证实了审计发现的那**第二处**
  同类缺陷（这次是**先证实、再下结论**）：

  - 父进程把整页填成 `i ^ 0x5A` 图案，把页内偏移 64 处的 futex word 置 **0**（未锁），然后 `fork()`；
  - 子进程 `futex(FUTEX_LOCK_PI)`（PI 锁会把 owner TID 写进该 word），随后**持锁退出**（不 unlock）；
  - 父进程 `wait4()` 后检查自己那一份：word 应仍为 **0**、整页图案应原样。

  正确实现会在写之前破坏 COW（写落到子进程的私有副本）⇒ 父进程不受影响；而 A20OS 的
  `futex_user_word_map()` 返回的是**裸帧指针**、`__atomic_store_n()` 直接写它 ⇒ 写进两者共享的那一帧
  ⇒ 父进程的 word 被改写。

  ```
  宿主（参照）：pi 00000000 clean      pi-control 00000000 clean   # 5/5 全干净
  客体（A20OS）：pi 00000001 POLLUTED   pi-control 00000000 clean   # 20/20 全 POLLUTED
  ```

  - `bad=1` 正是「word 不再是 0」这一项坏掉（图案本身完好）⇒ **父进程的 futex word 被跨进程改写**；
  - `pi-control` 轮（子进程**先**脏了该页、COW 已由子进程自己破坏）客体**也是 clean** ✓
    ⇒ 与 cleartid 一样，失效点**精确地**是「**内核没有为这次写破坏 COW**」；
  - 复现率 **20/20（确定性）**、宿主全对 ⇒ **第二个真实的跨进程内存损坏 bug，已证实**。
  - 复现器：`/tmp/opencode/pitest.c`（静态、无 libc、宿主客体都能跑）。

  ⇒ 这确认了上一条的**审计推断是对的**。**但它仍未修复**：修法必须「在取 `mm->lock` 之前完成 COW 破坏」
  （原地调用 `handle_cow_fault()` 会自死锁），且此处需要**原子写**，不能用 `copy_to_user()` 替代。

  **修复方案（插入点已找好，供下一步实施）**：
  - **调用点**：`kernel/abi/linux/sys_futex.c:49` / `:51` / `:63` 三处 `futex_pi_acquire()`
    （`FUTEX_LOCK_PI` / `FUTEX_TRYLOCK_PI` 及相邻分支）—— 它们都在**引擎取 `mm->lock` 之前**，
    正是可以安全破坏 COW 的位置；
  - 核心里**没有**现成的「为写而破坏 COW」独立 helper（`user_resolve_leaf()` 里那段是 `static`），
    所以需要把那段逻辑（叶子不存在 → `handle_demand_fault()`；存在但不可写 → `handle_cow_fault()`）
    **提炼成一个可导出的 helper**，在这几处**先调它、再进引擎**；
  - 做完之后，`futex_user_word_map()` 返回裸帧指针才是安全的（此时该页已是本任务私有），
    并且仍需保留原子写（不能换成 `copy_to_user()`）。
  - **验证方式**：用同一个 `pitest` 复现器 —— 修复后客体应当变成
    `pi 00000000 clean`，与宿主一致（就像 cleartid 那次 `20/20 POLLUTED → 20/20 clean` 一样）。

  **✅ 已按上述方案修复并验证**：
  - **实现**：在 `kernel/mm/mm.c` 新增可导出的 `user_prepare_write(task_t*, uint64_t)` —— 把
    `user_resolve_leaf()` 里那段「叶子不存在 → `handle_demand_fault()`；存在但不可写 →
    `handle_cow_fault()`；最后确认可写」的逻辑独立出来，并在 `kernel/include/sys/usercopy.h` 声明；
    然后在 `kernel/ipc/futex.c` 的 **`futex_pi_acquire()` 循环顶部**与 **`futex_pi_release()` 取锁之前**
    各调用一次 —— 即在**取 `mm->lock` 之前**完成 COW 破坏，之后 `futex_user_word_map()` 返回的裸帧指针
    才是安全的。放在引擎里（而不是 Linux ABI 的 `sys_futex.c`）可**同时覆盖 Native ABI**。
  - **编译踩坑**：`usercopy.h` 原来不认识 `task_t`，原型里的 `struct task_t *` 被当成「在参数表里声明
    新 struct」，`-Werror` 直接报错；在头里补一句 `struct task_t;` 前置声明即可。
  - **验证（同一个 `pitest`、同一个镜像，只换内核）**：
    ```
    修复前：pi 00000001 POLLUTED   pi-control 00000000 clean   # 20/20 与 20/20
    修复后：pi 00000000 clean      pi-control 00000000 clean   # 20/20 与 20/20 ← 与宿主逐项一致
    ```
    ⇒ 20/20 的跨进程污染**全部消失**，与宿主（Linux，5/5 clean）一致。
  - **残留（诚实记录）**：`user_prepare_write()` 是在取锁**之前**破坏 COW，因此「prepare 之后、
    锁内存储之前」若恰好发生 `fork()` 使该页重新变成 COW，理论上仍有**极窄的竞态窗口**。
    要彻底关闭它，需在 `mm->lock` 内**复查 PTE 可写性**（不可写则解锁重试）；本轮未实施。

- **【第三处 + 第四处同类缺陷】信号投递路径也在「拿裸帧重新映射」；另有影子栈 token 一处**

  沿同一 bug 类继续审计 `pt_translate` / `pfn_to_virt` 的用法，又找到两处：

  - **`kernel/proc/signal.c` 的 `signal_make_page_exec()`（26-36 行；唯一调用点 796 行）** ——
    **信号投递路径**：为了让栈上的 sigreturn trampoline 可执行，它 `pt_translate` 取出**物理帧**，
    然后 `pt_unmap()` + `pt_map(同一个 pa, ...writable_dirty_exec...)`，即**把同一个帧重新映射成
    可写可执行**。要注意：**信号帧本身是用 `copy_to_user()` 写的（790/793 行）✓**，所以帧写是安全的；
    危险的是**这个重映射** —— 若该页此刻仍与另一个任务 **COW 共享**，这一步会让本任务拿到
    **共享帧的可写别名**，此后本任务的任何写入都会改到**对方**那一份 ✗。
  - **`kernel/abi/linux/sys_missing.c:573-581`（CET 影子栈 token）**：`handle_demand_fault` 之后
    `pt_translate` + `pfn_to_virt` 直接 `*tok = ...` —— 同样是裸帧写（影子栈少见，但同类）。

  **为什么信号这处最可疑**：`signal_make_page_exec()` 在**每次投递信号**时都会执行，而它操作的页就是
  **用户栈**；`fork()` 之后父进程的栈页与子进程 **COW 共享**，而 mpv（ytdl 助手）、tumblerd（GLib 起助手）
  都会 **fork 子进程、并在子进程退出时收到 SIGCHLD** ⇒「**信号投递 + 刚 fork 过的栈页**」正好凑齐。
  这比 futex PI 那条（触发面很窄）更有可能解释那几条偶发崩溃。
  （**谨慎**：本轮做的是代码定位与修复，症状层面的因果**尚未**用端到端方式证实，故措辞按「可疑」处理。）

  **修法（本轮已实施）**：在 `signal_make_page_exec()` 的 `pt_translate` **之前**调用
  `user_prepare_write(t, page)` —— 先确保该页是本任务私有（必要时破坏 COW），
  之后的 unmap/map 操作的是**私有帧**，不会再产生共享别名。与 cleartid / futex 两处同一修法。

  **⚠️ 更正（同一轮验证的结果）**：修好之后，用**同一套症状负载**（60× `java -version` +
  120× `mpv --vo=null`）在**修复后的内核**上再跑一遍，症状**依旧复现**：
  ```
  SIGSEGV: pid=925 code=13 sepc=0x638a6320 stval=0x8
  FATAL:   pid=925 signal=11 abi=0 pc=0x638a6320 sp=0x77dbbc40 comm=lua/console path=/extra/usr/bin/mpv
  ```
  ⇒ 上面那句「更有可能解释那几条偶发崩溃」是**过度推断，在此撤回**：
  信号这一处**确实是同类缺陷、也确实该修**（已修），但它**不是**那些偶发崩溃的原因。
  - **同一轮的回归探针全部干净**：`ctidtest` 5/5、`cowtest` 5/5、`pitest` 5/5
    ⇒ 说明这一轮内核改动**没有破坏** COW / cleartid / futex 这几条路径 ✓。
  - ⇒ 至此**同类缺陷共找到 3 处、修了 3 处**（cleartid、futex PI、signal），
    但 mpv / LuaJIT / tumblerd 那几条症状**仍未解释**。

- **【MC 的 OpenAL 有结论了：不是内核 bug，而是「把 glibc 版原生库塞进 musl 客体」】**

  用和 `ld-musl+0x4602b` 相同的办法（把 `__cxa_throw` 的调用方地址减去它所在 mapping 的基址），
  拿到了抛出点的**稳定偏移**：**`libopenal.so + 0xf168`**（多轮都落在这个偏移上，低位一直是 `...168`）。
  顺着这条线查下去，根因清楚了：

  - 抛异常的 `libopenal.so` 是 **LWJGL 自带的 natives**（客体里
    `/usr/share/a20-media/1.21.11/natives/libopenal.so`，运行时被 LWJGL 解到
    `/tmp/lwjgl_root/3.3.3+5/x64/`）。把它从镜像里抠出来看 `NEEDED`：
    ```
    libdl.so.2  libstdc++.so.6  libm.so.6  libgcc_s.so.1  libpthread.so.0  libc.so.6
    ld-linux-x86-64.so.2
    ```
    ⇒ **这是一个 glibc 构建的库**（`ld-linux-x86-64.so.2` / `libc.so.6`），**不是 musl 的** ✗；
    而 `libstdc++.so.6` 出现在 `NEEDED` 里 ⇒ 抛 `std::system_error` 的 C++ 代码就在**这个库内部** ✓。
  - 它能被加载起来，是因为客体装了 Alpine 的 **gcompat**（`/lib/libgcompat.so.0`，以及
    `/lib/ld-linux-x86-64.so.2`、`libc.so.6`、`libpthread.so.0`、`libdl.so.2`、`libm.so.6` 这些
    glibc ABI 的桩）；这个库**一共从 glibc ABI 导入 283 个符号**。
  - ⇒ **结论：MC 的 OpenAL 失败属于「glibc 版原生库 + gcompat 兼容层」这条链**，
    而不是 A20OS 内核缺陷 —— `std::system_error` 正是 C++ 在 `pthread`/`mutex`/`std::thread`
    一类调用拿到错误码后抛出的典型形态，而这类调用在 gcompat 上最易失真。
  - **可行动方向**：改用 **musl 构建**的 OpenAL / LWJGL natives（或让 LWJGL 指向 musl natives），
    或补齐 gcompat 在这一路上的语义 —— **不必再往内核里找**。

  （附注：`+0xf168` 落在该库的第一个 LOAD 段内，且该库已 strip；要继续点名具体函数，
  需要用它的 `.eh_frame` 或带符号构建，但对「glibc/gcompat」这个结论已非必要。）
- 影响：**JVM 可用**，所以 Minecraft 的第一障碍其实是 **GL 链**（IN_FORMATS → PRIME → GL 渲染器）；
  剩下的是 mpv 那条 1/10 的多线程内存问题（会影响长跑的 Java 游戏）。
- **2026-09 更新（信号对齐 + ucontext 布局修掉后）**：`68abf68f`（handler 入口对齐）之后 JVM 能跑 handler、
  能打印崩溃报告；暴露出的真正崩溃是 **HotSpot 的隐式空指针检查**（`SHA5.implCompress0` 的数组访问依赖
  「空数组 fault → 处理器抛 NPE」，A20OS 上没被识别 → JVM 当致命崩溃）。最小复现器（宿主 javac 编 `.class`
  注入客体）显示 `o.hashCode()` 能正常抛 NPE、`arr[0]` 直接把 JVM 打崩。用
  `-XX:+UnlockDiagnosticVMOptions -XX:-ImplicitNullChecks` 可规避（`java -cp client.jar Main` 不再崩，
  只在缺失的 `joptsimple` 上报 `NoClassDefFoundError`）。
- **ucontext/siginfo ABI（均已修）**：A20OS 的 ucontext 与 Linux/musl 不一致（`uc_sigmask` 排在
  `uc_mcontext` **前**、mcontext 私有寄存器序、FPU 内嵌而非 `fpregs` 指针），已按 musl 对齐
  （`_Static_assert` 钉死偏移与尺寸；`2c1d0dbe`）；同步 SIGSEGV 的 siginfo 也改成 Linux 的
  `SEGV_MAPERR` + 故障地址（`4cf43e37`）。修完后 `bad uc->uc_mcontext.fpregs` 与错误报告自身 fault 都消失。
- **启动器两个 classpath bug（已修 `f3469887`）**：`classpath.txt` 是**单行冒号分隔**，启动器的贪婪
  `sed '^.*/libraries/'` 把整条 classpath 塌成一条，又把 client.jar 粘在无结尾冒号的 CP 后面——这正是
  最初那个 `ClassNotFoundException` 的来源；并加上 `-Dos.name=Linux`（LWJGL/Minecraft 拒绝平台名 "A20OS"）。
- **现状**：启动器能跑完整套真实启动流程、成功加载 **LWJGL 3.3.3+5** 并**开窗成功**。LWJGL 自带的
  `libjemalloc.so` 会 fault（`-Dorg.lwjgl.system.allocator=system` 绕过）；开窗时 X11 曾报
  `XIO: fatal IO error 90 (Message too large)`，根因是 A20OS 的 AF_UNIX 旧队列路径把单条消息卡在
  `NET_MAX_PAYLOAD`(64KiB)，**已修（`4e2aeb9f`）**：STREAM 大写按 64KiB 分片。现在卡在 **DNS**
  （`UnknownHostException: api.minecraftservices.com`，客体未配 resolver）。
  完整过程与证据见 [minecraft.md](minecraft.md)。

### x86_64 可用 RAM 曾硬编码成 1 GiB（**已修复**：改从 multiboot 内存图取）
- 事实：`kernel/arch/x86_64/include/platform.h:8` 把 `PHYS_MEMORY_END` 写死为 `0x40000000`（1 GiB），
  `arch_ram_range()`（同文件 :17）只返回 `[0, 1 GiB)`；所以即便 QEMU 给 `-m 2G`，内核仍只管理 262144 个页框
  （启动日志 `[PFA] total_frames=262144`、`[MM] ... 256061 free (1000 MB)`）。
- 高位直接映射**已经**覆盖物理 0–2 GiB（`kernel/arch/x86_64/boot/entry.S:74-77` 用两个 1GB 大页映为 RAM/WB），
  所以扩大上限只缺"知道真实 RAM 大小"。正确做法：在 `_start`（32 位段、`cld` 之后、清 BSS 之前）把 multiboot
  的 magic(EAX) 与 info 指针(EBX) 存进 `.data` 全局，再解析 `multiboot_info.mem_upper`；或走 fw_cfg 的 `etc/e820`。
  **注意**：1–2 GiB 区在 q35 上可能含 ACPI/固件保留区，不能直接整段当可用帧，否则分配器会把保留页发出去。
- 关联（待验证，非结论）：JVM 不带 `-Xms/-Xmx` 时报 `Too small maximum heap`（`/usr/bin/java` 封装已用
  `-Xms256m -Xmx512m` 绕过），疑似与其读到的内存 ergonomics 有关；1 GiB 对 Minecraft 也偏紧。
- **修复（`884ef373`）**：`_start` 现在在清 BSS 前把 multiboot magic(EAX)/info(EBX) 存进 `.data`，
  `firmware.c` 解析 multiboot 内存图：取 type 1（available）区间、丢掉低于 1 MiB 的部分（IVT/BDA/legacy hole）、
  按页对齐、并裁到 `entry.S` 直接用 1GB 页映射的 2 GiB 窗口；取不到就回退旧的 1 GiB，不回归。
  实测客体：`[RAM] usable 0x100000..0x7ffd9000 (2046 MiB)`、`[PFA] total_frames=523993`（原 262144）、
  `Buddy+Slab ... 2020 MB free`，桌面与 ffplay 照常。

### 视频播放：已默认用 ffplay（mpv 在 A20OS 上不可靠）
- **现状（已落地，`7ff4b14c`）**：`ffplay` 现在是视频 MIME 的**默认处理器**（overlay 里 `ffplay.desktop`
  + `/etc/xdg/mimeapps.list`；客体 `xdg-mime query default video/mp4` → `ffplay.desktop`），双击视频即用它播放。
  实测客体播 5s H.264 全片跑完（窗口路径、`-autoexit` 干净退出），`ffmpeg` 解码同一文件也是 0 错。
- **mpv 为什么不可靠**：它把每个内建 Lua 脚本（osc/stats/console/…）跑在**各自线程**里，会踩到上面那条
  per-thread 状态 bug → SIGSEGV。`load-scripts=no` **并不能**关掉这些内建脚本（崩溃只是从 `lua/stats` 换成了
  `lua/console`）；而旧 conf 里的 `stats=no` 是**无效选项**（mpv 0.40 没这个 option），会让 mpv 每次启动报错。
  已删掉那行非法配置、保留 `load-scripts/osc/ytdl=no` 作缓解，但 mpv 仍不可靠——**用 ffplay**。
- **两个注意点**：`ffplay -nodisp`（无显示）会 `Failed to open file ... or configure filtergraph`（无显示路径的
  怪癖，**窗口路径正常**）；被 SIGTERM 杀掉时 ffplay 会 SIGSEGV 退出（播完或 `-autoexit` 不会）。
- 用法：双击视频文件，或 `Super+Enter` 开终端 → `ffplay -autoexit /usr/share/a20-media/<视频>`。

### JVM：需要 `/usr/bin/java` 封装（exec_path 不解析符号链接 + 堆参数）
- 现象：`java -version` 报 `Error loading shared library libjli.so: No such file or directory (needed by java)`；直接用真实路径
  `/usr/lib/jvm/java-21-openjdk/bin/java -Xms256m -Xmx512m -version` 则正常输出 `openjdk version "21.0.12"`。
- 根因链（已核实）：`libjli.so` 在 `/usr/lib/jvm/java-21-openjdk/lib/`，`java` 二进制靠 RPATH `$ORIGIN:$ORIGIN/../lib` 找它；
  musl 的 ldso 用 `readlink("/proc/self/exe")` 展开 `$ORIGIN`（`user/external/musl/ldso/dynlink.c` 的 `fixup_rpath()`）；
  而 A20OS 的 `/proc/<pid>/exe` 原本是**普通文件**（`readlink` 返回 `-EINVAL`），且 `exec_path` 只做「绝对化 + 归一化」、
  不解析符号链接（`kernel/proc/exec.c:846` 起），于是 `$ORIGIN` 停在 `/usr/bin`。
- 已修：`/proc/<pid>/exe` 与 `/proc/<pid>/cwd` 现在是 magic symlink（`kernel/fs/procfs/procfs.c`：vnode 类型 +
  `procfs_readlink`）。实测 `ls -l /proc/self/exe` 已显示 `-> /bin/ls`。
- 仍未修：`exec_path` 不解析符号链接，所以经 `/usr/bin/java`（符号链接）启动时 `$ORIGIN` 与 libjli 推导的 `java.home`
  都落在 `/usr/bin`。**正解**是在 `kernel/proc/exec.c` 记录 `exec_path` 时做 realpath（`vfs_resolve`）；当前用 overlay 的
  `/usr/bin/java` 薄封装绕过：直接 exec 真实路径。
- 另有堆参数问题：不显式给 `-Xms/-Xmx` 时 VM 报 `Too small maximum/initial heap`（guest 内 `MemTotal` 1048576 kB、
  `MemAvailable` 541068 kB）。封装默认 `-Xms256m -Xmx512m`，命令行参数仍可覆盖。
- 提示：`MemTotal` 只有 1 GB（该实例 QEMU 给的是 2G）也值得单独核对 `pfa.total_frames` 的口径。

### 桌面壁纸不显示（backdrop 不渲染）
- 症状：xfdesktop 在跑（桌面图标正常、root 被背景层覆盖），但桌面是纯黑；1024x768 下 94% 像素为 `(0,0,0)`。
- 已核实（第一轮）：`/backdrop/screen0/monitor<id>/workspace0/last-image` 的 `<id>` 用的是 `sha1(connector)`，而 `sha1("Virtual-1")` **正好在** overlay 的 9 个 monitor key 里，所以不是 key 不匹配；`image-style=5`、`color-style=0` 合法；改成 SVG 壁纸并显式加 `image-show=true` 后**仍然全黑**。
- 镜像里 **没有 jpeg/png 的 gdk-pixbuf loader**（`loaders.cache` 有 ani/bmp/gif/icns/ico/pnm/qtif/svg/tga/tiff/xbm/xpm），`gdk-pixbuf` 也不依赖 libjpeg/libpng，所以 `xfce-blue.jpg` 本来就无法解码（这也是已改指向 `xfce-flower.svg` 的原因）。
- **guest 内实测（第二轮）**：
  - `xfconf-query -c xfce4-desktop -lv` 里 backdrop 树**完全正确**：`monitor0/workspace0` 与 9 个 `monitor<sha1(connector)>/workspace0` 全是
    `image-show=true` / `image-style=5` / `last-image=/usr/share/backgrounds/xfce/xfce-flower.svg` / `color-style=0`；
  - SVG 文件确实存在（16972 B），`libpixbufloader_svg.so` 在 loaders.cache 里注册了，`librsvg-2.so.2`(2.61.2) 也在镜像里。
  - 也就是说**解码链（gdk-pixbuf → svg loader → librsvg）应该可用**，`last-image` 指向的文件也在，但画面仍全黑。
  - 所以第二个原因在**渲染侧**：xfdesktop 的 backdrop 窗口（layer-shell 表面）到 labwc(WLR_RENDERER=pixman) 合成这一段，或 xfdesktop 对 SVG 的 rasterize 失败。
- **第三轮（二分 decode vs render）**：把壁纸换成一个 `loaders.cache` 一定支持的 BMP（宿主造了张纯红 64×64 BMP，`debugfs` 注入 `/root/red.bmp`，两个 monitor key 都改成它并 restart xfdesktop）——
  截图里**红色像素为 0**，画面还是纯黑。也就是说**连 BMP 都不渲染**，问题不在解码，而在「backdrop 上图 / 合成」这一段。
  另外同一轮里出现了 `SIGSEGV: pid=163 code=13 stval=0x12`（用户态空指针附近访问），很可能是 xfdesktop/缩略图相关组件在画 backdrop 时挂掉。
- 下一步（渲染侧）：`xfdesktop` 加 `--enable-debug`/`G_MESSAGES_DEBUG=all` 看 backdrop 的加载与绘制；确认 backdrop 的 layer-shell 表面是否真的 `commit`（labwc 日志 / `wayland-info`）；并查那个 `stval=0x12` 的 SIGSEGV 属于哪个进程。
- **第四轮（xfdesktop 全量 debug）**：`G_MESSAGES_DEBUG=all xfdesktop` 输出里**完全没有 backdrop/image/draw 相关日志**，只有 dconf/GTK/GIO 的常规初始化（`Compositor prefers decoration mode 'server'`、`Connecting to session manager`、没有 session manager/portal、`mnt_monitor_get_fd failed: Operation not permitted`）。也就是说 xfdesktop **根本没走到「加载并绘制 backdrop」**（或那一段无日志），backdrop 表面上没有被真正画出来。
- **第五轮（xfdesktop 版本/协议）**：镜像是 **xfdesktop 4.20.1**，链接了 `libgtk-layer-shell.so.0`；二进制里有 `Your compositor must support the zwlr_layer_shell_v1 protocol` 这条串，说明它的 Wayland backdrop 依赖 wlr-layer-shell。但日志里**并没有**这条报错，也没有任何 backdrop 行；同时 `xfdesktop --version` 正常、xfce4-panel 正常。
  → 收敛判断：**xfdesktop 4.20.1 的 Wayland backdrop 很可能根本没被绘制**（该版本 Wayland 支持较新且有缺口），而不是协议缺失或解码失败。可行修法：改用**合成器层面的背景工具**（`swaybg`/`wbg` 之类，仅需一张图并挂 background 层），或在 xfdesktop 里确认 Wayland backdrop 的开关/补丁。镜像里目前**没有** swaybg/wbg/hsetroot/feh 任何一个。
- **第六轮（改用合成器层背景工具 swaybg）**：按第五轮的修法把 `swaybg`(1.2.1) 加进 `xfce` world 并在
  `start-desktop-components.sh` 里启动，重制镜像后验证。swaybg **确实起来了并配置成功**
  （`[main.c:282] Found config * for output Virtual-1 (AOS A20OS Display 0x00000001)`，进程存活 12s+ 未退出），
  但截图里**依然没有背景**：
  - `swaybg -i /usr/share/backgrounds/xfce/xfce-flower.svg -m fill` → 桌面区仍是纯黑；
  - `swaybg -m solid_color -c 00aa22`（纯绿）→ 绿色像素 **0**；
  - `pkill -x xfdesktop` 之后再截图 → 仍是纯黑（排除「xfdesktop 用黑表面盖住背景层」）。
  两种模式（图片 / 纯色）都失败，说明**不是解码、也不是模式选择**，而是
  **labwc 没有把 background 层的 wlr-layer-shell 表面合成进 scanout**；同一会话里 panel 所在的层却正常渲染。
  → 收敛：壁纸真正的阻塞点是 **labwc / wlr-layer-shell 的 background 层合成路径**，不是「缺一个背景工具」，
  也不是 xfdesktop 的 backdrop 开关（第四、五轮的结论需要按此修正）。下一步：在 labwc 侧看 background 层表面的
  map/commit 与 pixman renderer 的 damage 处理，单独排除 `WLR_RENDERER=pixman`、`WLR_NO_HARDWARE_CURSORS=1`、
  `WLR_DRM_NO_ATOMIC=1` 三个开关。
- **第七轮（时间维度采样：上面第六轮的结论是错的）**：第六轮的截图都是在启动 ~60s 之后拍的，而壁纸在那之前就
  已经**画出来过又被盖掉**。用户观察「刚启动有壁纸，过一会儿变黑」后按时间重采（headless，每 5s 一张，
  统计桌面区非黑像素）：

  | t(s) | 颜色数 | 最大占比色 | 黑像素 |
  |---|---|---|---|
  | 5–20 | 1–2 | (0,0,0) | ~100% |
  | **25** | **202** | **(0,144,188)** | **0.0%** |
  | 30+ | 26 | (0,0,0) | 99.4% |

  即 **swaybg 的壁纸确实画上去了（t=25s），5 秒后就被纯黑盖掉**。
- 决定性实验：把 `spawn xfdesktop` 换成一句 echo（不启动 xfdesktop）后重采，壁纸从 t=25s **一直保持到
  t=110s**（202 色 / 0% 黑）。→ **凶手是 xfdesktop**：它不画配置的 backdrop，而是铺了一块**不透明的黑色
  桌面表面**，把 background 层上的壁纸整个盖住。（第六轮「`pkill -x xfdesktop` 之后仍是纯黑」之所以
  误导，是因为那时黑面已经合成进 scanout，杀掉进程并不会让合成器回到上一帧。）
- **修法（已验证）**：xfdesktop 留着（桌面图标照常），但让 swaybg 在**它之后**再创建自己的 background
  表面 —— wlr-layer-shell 同一层内后创建的表面在上面。实测「等 xfdesktop 进程出现 + settle 20s 再起
  swaybg」：壁纸从 t=54s 稳定保持到 t=144s（229 色 / 0% 黑），面板与图标同时在。
  代价：启动后约 30s 内桌面是黑的。若日后能把 xfdesktop 的 Wayland backdrop 修好、或让它别铺那块黑面，
  这段等待就可以去掉。
- 顺带：同一批次里 `tumblerd` 以 `code=1`(#GP, `insn 0f b6 48 ..`) 崩溃多次（`FATAL: pid=... comm=tumblerd`），是独立的用户态坏指针崩溃。
- **同一轮抓到一个确定的用户态崩溃**：`FATAL: pid=140 ... comm=tumblerd`，内核侧是
  `ADE/ALE: pid=140 sepc=0x40226031 stval=0x8136a00e code=1` —— 又是 **#GP**（`code=1`，x86_64 上只由 #GP 产生；`stval` 对 #GP 是过期 CR2），`insn@sepc=0x48b60ff4`（`0f b6 48 ..` 一个字节 load）。即**缩略图守护进程 tumblerd 在解引用一个坏地址而挂**（xfdesktop 用它给桌面图标出缩略图）。这与 JVM 最初那条 #GP 同类（非规范地址/坏指针），是**另一个独立的用户态崩溃**，值得单独查。

### guest 写盘后 ext4 位图校验和不一致
- 症状：`image-world` 产物 `e2fsck -fn` 干净；但 guest 启动一次后，从宿主机看（QEMU `-snapshot` 的 qcow2 overlay，或 kill 后的镜像）会出现 `Block bitmap checksum does not match bitmap`，`debugfs` 直接打不开。
- 意义：这解释了本文档「测试环境注意事项」里那条「损坏来源未定位」——**损坏不是构建期引入的，是 guest 写入时产生的**。损坏的 rootfs 会伪造内核 bug（udev worker 超时、应用起不来），排查前务必先 `e2fsck -fn`。
- 提示：怀疑 A20OS 的 ext4 写入路径没有同步 `metadata_csum` 的位图校验和（`^has_journal` 下没有日志回放来兜底）。需要在内核 ext4 写路径上核对 group descriptor/bitmap 的 checksum 更新。

### 面板下拉菜单项不启动应用
- 症状：面板按钮能开菜单，但点下拉菜单项既不启动应用（无新 `xdg_surface`、无窗口），也不关菜单。
- 现状（在 `e2fsck` 干净的 rootfs 上复验）：指针移动与坐标正常——悬停分类项会弹出子菜单；点击面板按钮会打印 `press on layer-(sub)surface`。
- 注意：这与上面已解决的「窗口控制键无效」是**两个不同症状**——窗口控制/聚焦/拖动已由 labwc 默认绑定修复，本条的客户端菜单条目点击仍未定位。
- 已排除：内核输入通路本身。`struct input_event` ABI、PS/2 按键解码、每包 `SYN_REPORT`、evdev 打戳、内核与 vDSO 的 CLOCK_MONOTONIC 时基逐项核对一致；libinput 也枚举到了设备（`Adding A20OS evdev mux [0:0]`）。
- 提示：是否打日志取决于命中节点的类型（只有层表面分支会打印），所以「落在子菜单上的按下不打印任何 labwc 日志」本身是正常的，不能当作事件没送达的证据。下一步应在 guest 内直接看 `wlr_scene_node_at` 的命中结果与客户端实际收到的 `wl_pointer.button`，不要再从截图推断。labwc 会把每个执行到的 action 记进日志（`[action.c] Handling action <n>: <Name>`），这是比截图可靠的观测手段。

### dbus 同步调用超时 / 对端 pid 为 -1
- 症状：会话里 `NoReply`，xfsettingsd/panel 报 Xfconf/dconf 超时后降级；dbus 日志里身份异常：`dbus-daemon[44]: [session uid=0 pid=18446744073709551615 pidfd=5] ...`（pid 为 -1）。
- 现状：在 `e2fsck` 干净的 rootfs 上稳定复现。同一行里 `requested by ':1.7' (uid=0 pid=92 comm=".../xfsettingsd")` 的 pid 却是对的。
- 已修（SO_PEERCRED 部分）：`kernel/net/socket_unix.c` 的 connect 路径把**发起端自己**的 pid 写进了 `s->peer_pid`，而发起端的对端应是服务端。现在 bind 时记录属主凭据（`net_socket_t.owner_pid/uid/gid`），connect 时用它给发起端填 `SO_PEERCRED`；accept 出的子 socket 那一路原本就对。
- 仍未修：改完后 dbus 日志里的 `pid=18446744073709551615` 依旧。同一行 `pidfd=5` 说明 `SO_PEERPIDFD` 已经拿到 pidfd，所以 dbus 显示的这个 `pid=` **不是**取自 `SO_PEERCRED`，而是 `SO_PASSCRED` 的 `SCM_CREDENTIALS`（`ch_cred_*` 路径）。下一步查 AF_UNIX 首包（dbus 的初始 NUL 字节）的 SCM_CREDENTIALS 为什么是 -1。
- 已排除：`peer_pid` 未初始化（`net_socket_alloc()` 用 `obj_cache_alloc_zero()`）；`getpid()` 本身（`dbus-daemon[44]` 说明任务 pid 正确）。

### 其他架构的 FPU/上下文
- 状态：riscv32、ppc64le 已补上 trap 帧的 FP 保存（ppc64le 仅编译验证）；**arm32 仍未修**（本机无 arm 工具链，且其 trap 帧与 trap.S 用逐字段断言强绑定，改动未经验证）。
- 提示：判定标准是「用户态是否启用 FP/SIMD」——arm32（`-mfpu=vfpv3-d16 -mfloat-abi=hard`）、riscv32（`ilp32d`）、ppc64le（`MSR_FP`）都启用；loongarch32/armv7m 无 FPU，不受影响。ppc64le 的向量保存受 `MSR_VEC` 门控而该位未设，需另存 FPR。

### 32 位架构构建
- 状态：riscv32 在 HEAD 上有多处既有编译错误（`arch_vdso_counter` 声明、`pfa_range_t` 断言、`proc.c` type-limits 等）；前两处已修，`proc.c` 等仍待处理。
- 提示：非 VDSO 架构（riscv32/arm32/loongarch32）都会撞到 `arch_vdso_counter` 声明缺失。

## 三、测试环境注意事项

- **测试前先 `e2fsck -fn` 校验镜像**：损坏的 rootfs 会**伪造出内核 bug**。实测一个报 `Block bitmap checksum does not match`、`Entry 'sys' ... unused inodes area` 的镜像，会让 6 个 udev worker 全部 `timeout; kill it`；换干净镜像后为 0。损坏镜像还会造成 `failed to create cairo scaled font`、应用起不来等假象。**新 `image-world` 产物 `e2fsck -fn` 是干净的**（5 个 pass 全过），说明损坏不是构建期引入、而是之后发生，具体来源未定位。
- **rootfs 构建**：直接 `apk add` 走官方 CDN 会慢到像卡死；用预置的 `a20rootfs-builder` docker 镜像（apk 源已指 USTC），约 2 分钟一个镜像。详见 [`build.md`](build.md)。
- **改 overlay 必须重建镜像**：debugfs 对 8GiB 的 metadata_csum ext4 打不开 rw。
- **QEMU 串口日志在后台跑时的坑**：镜像被 QEMU 以 RW 打开，重复启动前要清掉占用进程；判断占用者看 `/proc/*/fd`，不要用 `pgrep -f`。
- **GUI 设备的 QEMU 参数**：桌面 boot 需带 virtio keyboard/mouse/gpu 设备，否则没有 input/gpu class device。
- **宿主噪音**：宿主 systemd-udevd 偶发刷 `/sys/.../uevent: Permission denied`，与 A20OS 无关。

## 四、Backspace 失效（实测结论：内核输入路径已排除，故障在合成器/终端一层）

- **症状**：XFCE 桌面里 `xfce4-terminal` 按 Backspace 什么都不发生（连 `^H`/`^?` 之类的可见字符都没有）。
- **内核输入路径是对的**——这是本节的关键结论，且是实测不是推断。在 labwc 启动**之前**把
  `/dev/input/event0` 的原始流 dump 到串口（此时还没有第二个消费者，避开了 input_mux「单消费者」
  的抢占），按 a、b、Backspace、c、d 后把 24 字节的 `input_event` 记录逐条解码：

  ```
  01 00 | 1e 00 | 01 00 00 00    EV_KEY code=30 (KEY_A)         press
  00 00 | 00 00 | 00 00 00 00    EV_SYN
  01 00 | 1e 00 | 00 00 00 00    EV_KEY code=30                 release
  ...
  01 00 | 0e 00 | 01 00 00 00    EV_KEY code=14 (KEY_BACKSPACE) press
  00 00 | 00 00 | 00 00 00 00    EV_SYN
  01 00 | 0e 00 | 00 00 00 00    EV_KEY code=14                 release
  ```

  时间戳是正常的 CLOCK_MONOTONIC，每个事件都配了 `EV_SYN`。即 `ps2.a20drv`（set-1 scancode
  `0x0e` → evdev `14`）→ `input_mux` → `/dev/input/event0` 整条链全部正确。

- **端到端复现**：在终端里注入 `echo ab<Backspace>c<Enter>`，屏幕上是 `a20os# echo abc` 且输出
  `abc` —— letters/space/Enter 都到位，`b` 没被删掉。也就是说**那个字节根本没到 tty**（否则
  `stty erase = ^?` 的行规一定会删掉它）。
- **pty 的 ERASE 是实现了的**：`kernel/drivers/char/pty.c` 的 `pty_input_byte_locked()` 里
  `ch == c_cc[PTY_CC_VERASE]` 会 `canon_len--` 并按 `ECHOE` 回写 `"\b \b"`；客体里 `stty -a`
  报 `erase = ^?`。
- **客体 xkb 数据是完整的**：`keycodes/evdev` 有 `<BKSP> = 22;`、`symbols/pc` 有
  `key <BKSP> {[ BackSpace, BackSpace ]};`、`rules/evdev` 存在；labwc 日志打
  `[../src/config/keybind.c:136] Found layout English (US)`，说明布局解析成功。
- **已排除**：labwc 自己吞键（`root/.config/labwc/rc.xml` 只绑了 `W-Return`/`W-d`/`W-f`/`W-q`/
  `W-Escape`，没有 BackSpace）；`xkbcomp` 的 `Unsupported maximum keycode 708, clipping` 警告是
  **正常现象**（`keycodes/evdev` 本就声明到 708 而文件头 `maximum = 255`，任何发行版都有这条）。
- **判别实验（已做）**：在**非 VTE** 的 Wayland 客户端里试同一个键 —— `xfce4-appfinder`
  的搜索框里打 `ab<Backspace>c`，框里显示 **`ac`**（`b` 被删掉了）；而同一段按键序列在
  `xfce4-terminal` 里留下 `abc`。**即 libinput/labwc/xkb keymap 都是好的，问题只在 VTE。**
- **根因（已定位并修复）**：`kernel/drivers/char/pty.c` 的 `pty_slave_ioctl()` 实现了
  `TCGETS`/`TCSETS`，但 `pty_master_ioctl()` **没有**，会走到 `return -ENOTTY`。Linux 上 pty
  两端共享同一份 termios，而 VTE 系终端（xfce4-terminal）正是在 **master fd 上调
  `tcgetattr()`** 来解析它 `auto` 的 Backspace 绑定；拿到 `ENOTTY` 就学不到 erase 字符
  （客体里其实是 `^?`），于是 Backspace 一个字节都不发 —— 与「字母/回车正常、只有 Backspace
  毫无反应」完全吻合。
  **已修**：给 `pty_master_ioctl()` 补上 `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`，与 slave 共用
  `g_ptys[idx].termios`。实测同一段按键序列从 `a20os# echo abc`（`abc`）变成
  `a20os# echo ac`（`ac`）。
- **顺带纠正**：先前怀疑的 `input_mux` 伪造 `EVIOCGBIT(EV_KEY)`（`k = 1..0xff`）**不是**本例的
  原因 —— 同一设备上非 VTE 客户端的 Backspace 正常。该伪造仍不干净，但与本 bug 无关。
