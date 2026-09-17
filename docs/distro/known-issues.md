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
