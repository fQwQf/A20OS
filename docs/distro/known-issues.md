# 已知问题与排查笔记

桌面目前能起来、能渲染、键鼠可用。

## 一、已解决

### 输入：libinput 枚举不到设备

libinput 的 udev 后端报 "no input devices"，桌面完全没有输入。ABI 上缺两处：netlink uevent 缺
`SEQNUM=`（`kernel/net/socket_netlink.c`）；`/dev/input/eventN` 与 `/sys/dev/char/<maj>:<min>` 的
syspath 不一致（devfs + sysfs）。

### 间歇性读路径自死锁

高 I/O 阶段偶发整体挂死，串口 `LOCK-STALL` 显示 `owner==waiter`。两条相关约束：用户拷贝不得在
`vf->offset_lock` 持锁区内触发缺页；`mutex` 需要支持同 task 递归。

### 高 CPU / 桌面空转

桌面 idle 时宿主 CPU 100%，组件的事件循环在空转。x86_64 的空闲循环要 HLT；`epoll_ctl` 要按 Linux
`file_can_poll()` 拒绝不可 poll 的 fd；DRM/input/epoll 要提供 `poll_sources`，否则 readiness 只能
每 1ms 兜底唤醒。

### x86_64 桌面组件随机 SIGSEGV

xfdesktop/panel/thunar/gst 随机野指针崩溃，只在 x86_64 上出现。x86_64 要在上下文切换与信号帧
保存/恢复 FPU/SSE；aarch64/riscv64/loongarch64 的 trap 帧已经保存了 SIMD，缺这层时被抢占线程的
XMM 会被下一个任务覆盖。

### x86_64 信号处理器入口栈对齐错 8 字节（已修复：`68abf68f`）

任何使用 SSE 的信号处理器在入口即 `#GP`，内核反复重投 SIGSEGV，栈逐帧下压直到耗尽。串口刷
`ADE/ALE: ... code=1` + `insn@sepc=0x...`（如 JVM 读 jar 时 `0x418739a0` 的 `movaps [rsp+X], xmm0`）。
`code=1` 是 `CAUSE_INSN_FAULT`，x86_64 上只由 #GP 产生（不是 #AC 对齐异常）；`stval` 对 #GP 是
过期 CR2，不是故障地址。

根子在投递路径：帧基被对齐到 16（`sp &= ~15`），handler 入口 `rsp` 恰好等于帧基，于是入口
`rsp ≡ 0 (mod 16)`；而 x86_64 SysV ABI 要求函数入口 `rsp ≡ 8 (mod 16)`（相当于经 `call` 压了
8 字节返回地址）。handler 序言按标准 ABI 对齐栈做 SSE 溢出（`movaps [rsp+X]`），落在错位 8 字节的
地址上，触发 #GP；#GP 又发生在 handler 内，于是再投递、递归下去。

修法是保持帧基 16 对齐（内嵌 `fxsave64` 区域必须 16 对齐），handler 改在帧基下方 8 字节进入，
sigreturn trampoline 地址放在那儿供 handler 最后的 `ret` 弹出；专用 trampoline 页去掉补偿用的
`pushq %rax`。新加的 arch 钩子 `arch_signal_handler_sp()` 只对 x86_64 生效。

验证的结果是 JVM 读 jar 的 `ADE/ALE` 归零、其 SIGSEGV 处理器能正常打印崩溃报告；XFCE 桌面起来、
组件无 SIGSEGV、无内核 panic。


### thunar 运行一段时间后崩溃

GDK 报 `Truncating shared memory file failed: Out of memory`，随后 libwayland 空指针崩溃。wl_shm 池
是 memfd，其数据曾用单个连续 kmalloc 缓冲（1024x768x4 需 order-10 连续块），内存碎片化后
ftruncate 失败。现改为按需 order-0 页数组（`kernel/fs/memfd.c`）；页缓存写回/回收仍可继续观察。

### elogind 启动失败

`elogind-daemon: Failed to open pin file` 紧接 `Failed to allocate manager object`。elogind 在 manager
setup 阶段 pin 自己的 cgroup，路径取自 `/proc/1/cgroup`（`init.scope`），需要先建好该目录与
`/run/elogind`；建目录要用普通 `mkdir`，busybox 的 `mkdir -p` 会误判 sysfs 父目录为「Not a
directory」。

### thunar 缩略图服务缺失

`Thumbnailer1 was not provided by any .service files`，并反复
`Thumbnailer Proxy Failed ... re-initialize`。world 清单补 `tumbler`
（提供 `org.freedesktop.thumbnails.Thumbnailer1`）。

### overlay 配置：桌面图标、账号库、polkit 规则权限

桌面图标：`xfce4-desktop.xml` 里 `desktop-icons/style` 写成了 `1`（`XFCE_DESKTOP_ICON_STYLE_WINDOWS`
＝「最小化窗口图标」），而同一文件又配了 `file-icons`（只有 style `2` 才生效）。改成 `2` 后桌面出现
Home/Trash/Filesystem 图标。

账号库：overlay 的 `etc/passwd` / `etc/group` 在 `apk add` 之前落地，而 xfce world 不含
`alpine-base`，所以它就是账号库基底。原文件是 Debian 风格的极简集合（缺 `nobody`/`daemon`/`wheel`/
`games`…，且 `audio=29`/`video=44`/`kvm=47`/`input=290` 等 gid 与 Alpine 不符）。已按
`alpine-baselayout-data` 的规范集合重写，仅 root 的 shell 保留 `/bin/zsh`；内核 devfs 的设备节点 gid
恒为 0，不依赖这些 gid。

polkit：Alpine 的 polkit 包把 `/etc/polkit-1/rules.d/`、
`/usr/share/polkit-1/rules.d/50-default.rules` 打成 `0700 polkitd:polkitd`；`mkrootfs --usermode`
无法 chown，镜像里变成 `root:root 0700`，polkitd 读不到，会话日志出现 `Permission denied` +
`Error loading script .../50-default.rules`。`mkrootfs` 现在在 mkfs 的 fakeroot 会话里按 apk 归档
声明的 uname/gname 补回属主，会话日志已无该错误。

死配置：删除了 X11 时代的 `packages/overlay/xfce-x86_64/` + `packages/world/xfce-x86_64.world`
（`instances/xfce-x86_64.toml` 用的是共享的 `xfce` world），以及 devtools overlay 里无人引用的
`poweroff-helper`。

两条 XFCE 路径（`tools/a20 run xfce-*` 与 `make distro-run`）原本各带一份 rc.xml/session.conf/udev
规则；现在 `user/rootfs/alpine/build.sh` 也应用 `packages/overlay/xfce`（跳过其 `etc/`），会话层
单一来源。

### 点击异常：窗口控制键无效、点击不能聚焦

指针能动、面板按钮有响应，但窗口标题栏的最小化/最大化/关闭无响应，点击窗口本体不能聚焦/提升，
标题栏也不能拖动。根子在 overlay 的 `rc.xml`：它把 labwc 的内置默认绑定整表顶掉了。labwc 只读一个
`rc.xml`（XDG 顺序里的第一个，即 `/root/.config/labwc/rc.xml`），不合并 `/etc/xdg/labwc/rc.xml`；
而它的内置默认 key/mouse 绑定只在解析结果里 `rc.keybinds` / `rc.mousebinds` 为空时才安装
（`src/config/rcxml.c` 的 `post_processing()`）。overlay 里写了 5 个 keybind 和 1 个 `Root` mousebind，
于是 `rc.mousebinds` 非空，32 条默认鼠标绑定（`Client/Titlebar/Border Left Press → Focus+Raise`、
`Title Left Drag → Move`、`Close/Iconify/Maximize Left Click → Close/Iconify/ToggleMaximize`……）一条
都没装；keybind 同理，Alt-Tab/Alt-F4 也一起丢失。面板不受影响，因为它的按钮是客户端 GTK 自绘，labwc
只要有 `ctx.surface` 就照常转发指针事件，与 mousebind 无关。

修法是在 overlay 的 `<keyboard>` / `<mouse>` 里显式写 `<default />` 把内置默认拉回来，而且必须放在
前面：`deduplicate_*_bindings()` 保留后出现的定义，所以后面显式写的绑定（`W-Return` 等）仍然覆盖
默认值。

验证看 guest 日志：从"只有 `create mousebind for Root`、没有 `load default mouse bindings`"变成
`Loaded 32 merged mousebinds` + `Replaced 1 mousebinds`（Root Right 与默认重复被去重）+
`Replaced 1 keybinds`（`W-Return` 覆盖默认的 `lab-sensible-terminal`）；桌面里左键点击 root 触发了
`Handling action 15: ShowMenu`，这条绑定只存在于被拉回的默认表里。

涉及 `packages/overlay/xfce/root/.config/labwc/rc.xml`（`tools/a20 run xfce-*`）与
`user/rootfs/alpine/overlay/root/.config/labwc/rc.xml`（`make distro-run`），两处同一缺陷。

## 二、未解决

### java 退出码 255 / mpv 偶发崩溃（JVM 本身可用；多线程内存已定位，见本节末「根因已定位并修复」）
`java -version` 能打印完整且正确的版本号，但每隔一次就 exit 255（20 次里 9 次失败；另一次 6 次里
3 次失败，模式是 255/0/255/0），全程没有任何 SIGSEGV。mpv 那条是约 1/10 次崩溃
（`sepc=0x638a6320 stval=0x8`、`comm=lua/<script>`），反汇编为 LuaJIT 在 `libluajit+0x53320` 读
`*(G+0x130)` 得到 NULL，即 GC 链表头被写坏。

先看已经排除的：

- 单线程 LuaJIT 完全正常（6× JIT-on + 4× `-joff` 全过）；
- 上下文切换已保存/恢复 ra/tp/rbx/rbp/r12-r15/rsp/rflags/cr3 + `fxsave64`/`fxrstor64`
  （`kernel/arch/x86_64/boot/switch.S`）；
- 两条 trap 入口（`isr_common` 与 `syscall` 快速路径）都把 `MSR_FS_BASE` 存进 trap 帧 offset 184
  （`trap.S`），返回时写回（`trap.S` 尾部）；内核态嵌套 trap 走的是 `kernel_trap_handler`，不是
  `user_trap_handler`；
- `arch_prctl(ARCH_SET_FS)` 的落点没问题（都逐一验证过）：syscall 快速路径同样经 `trap_handler`
  → `user_trap_handler` 设置 `current->trap_ctx`（`kernel/arch/x86_64/trap/trap.S:411`、`kernel/core/trap.c:353`）；内核态嵌套 trap 走
  `kernel_trap_handler`；switch.S 的寄存器/FPU 保存完整。

**仍未定位。** 按 JVM 子系统分层看新证据：`java -version` 的退出码在三种配置下分别是
`plain: 255 0 255 0 255`、`-Xshare:off: 255 255 255 255 255`（每次都失败）、
`-XX:-UsePerfData: 0 255 0 255 0`（仍交替）。说明与 perf-data 无关，而与 CDS/类数据加载
（`lib/modules` jimage 的文件映射）强相关。

「文件数据/页缓存被读坏」是原来的首选假设，实测被否掉：

- 宿主侧用 `debugfs dump` 取出镜像里的 `java-21-openjdk/lib/modules`（142,510,592 B），
  md5 = `3d61e008965241fd1254b98b438184ee`；
- 客体内 `md5sum` 同一文件 3/3 次都返回同一个正确 md5，`dd bs=1M/64k/4k` 也都完整读满
  142510592 B；
- 即读路径（read/page cache）本身正确，JVM 不是被坏数据喂死的。

不过另一次 diag 里抓到了一个转瞬即逝的用户态崩溃：紧接着「`exit 7` / `kill -9 $$` / SIGSEGV」三个
进程退出测试之后，`md5sum` 同一个大文件时自己 SIGSEGV（core dumped），而同一次会话里再跑就正常。
这是偶发的内存损坏，与进程/线程的创建-销毁活动相关，不是文件内容问题。

退出状态编码本身是对的（`exit 7` → `$?=7`，`kill -9` → `137`），所以 `java` 的 255 是它自己真的
`exit(-1)`，不是内核把信号死亡报错。
- 「进程/线程退出清理写坏内存」也已排除。按上面的思路做了复现器：用 142MB jimage 的已知正确 md5
  （`3d61e008965241fd1254b98b438184ee`）当金丝雀，依次施加 `5× (java 后台 + kill -9)`、
  `5× java 正常退出`、`20× SIGSEGV`、`30× kill -9`，每一阶段后校验都 OK；内核日志里也没有
  OOM/oom-killer、没有 `[SHFAULT]`/坏 pfn 之类的记录。
- 大文件 mmap 也验证通过。用宿主 gcc 编了一个无 libc、纯 syscall 的静态 x86_64 小程序
  （`gcc -static -nostdlib -ffreestanding -fno-builtin -fno-pie`；同一二进制在宿主和客体都能跑），
  它对同一个 142MB jimage 同时做 `read()` 与 `mmap(MAP_PRIVATE)` 并各算一遍 FNV-1a：客体里 8/8 次
  `read fnv == mmap fnv == 宿主参考值 a14113a3dd22099e`（bytes=0x87e8a00=142510592）。读路径和
  mmap 缺页路径都正确，「文件映射/页缓存被读坏」这条彻底排除。
- 本轮最重要的修正是：JVM 其实没坏。加 `-Xlog:class+load=info` 后，`java -version` 在每一次运行
  （含 `-Xshare:off`）都加载到 `java.lang.Shutdown` / `java.lang.Shutdown$Lock` 并打印完整正确的
  版本号。JVM 每次都跑完了，只是退出码变成 255 而不是 0。所以「JVM 启动约 50% 失败」的说法不成立，
  JVM 本身可用（退出码不影响 Minecraft 启动）。
- 退出码 255 的来源在 `kernel/proc/wait.c:112-118`，它把 `exit_code` 映射成 wait status：
  `code >= 0` → `(code & 0xFF) << 8`，`code < 0` → `(-code) & 0xFF`。所以 `$?=255` 不可能来自
  `exit_code = -1`（那会得到 `$?`=1/129），只能是 `exit_code = 255`（或信号死亡的 127 编码），即 JVM
  launcher 真的返回了 255，与其 `DestroyJavaVM`/launcher 在「仍有线程没退干净」时返回 -1 的行为吻合。
  属于线程生命周期问题，不影响 JVM 计算。
- 本轮新增排除：进程/线程退出 churn（金丝雀 md5 四阶段全 OK）、OOM（内核日志无 oom-killer）、
  简单退出码编码（`exit 7`→7、`kill -9`→137），以及三条回收/映射路径（`mm_shared_file_fault` 的 pin、
  `swap_out_victim_pages` 的 PTE 替换、`MAP_PRIVATE` COW）。
- 并发线程互踩内存也已排除。另一个无 libc 静态小程序（raw
  `clone(CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM)`，4 个子线程各有独立 mmap 栈和 4KB canary
  缓冲，各自反复写/校验自己的 id 图案 30000 轮）在客体里 5/5 次全部 `RESULT: CLEAN`（每个线程
  mismatches=0），与宿主结果一致，线程之间不会互相写坏缓冲区。
- 退出路径探针是第三个无 libc 静态程序，`clone(SIGCHLD)` 每个变体 fork 一个子进程，父进程用 `wait4`
  打印原始 status。客体 3/3 次结果完全一致，与宿主参考对比：
  | 变体 | 含义 | 宿主 | 客体 |
  |---|---|---|---|
  | v0 | `exit_group(0)` | code=0 | code=0 ✅ |
  | v1 | `exit_group(42)` | code=42 | code=42 ✅ |
  | v2 | 3 个自旋线程 + 主线程 `exit_group(0)`（JVM 的退出模式） | code=0 | code=0 ✅ |
  | v3 | 3 个自旋线程 + 非 leader 工作线程 `exit_group(42)` | code=42 | **status=0xb、sig=11(SIGSEGV)、exited=0** ❌ |
  | v4 | `exit_group(255)` | code=255 | code=255 ✅ |
  | v5 | `exit_group(-1)` | code=255 | **status=0x1、sig=1** ❌ |
- v3 一度被记成本轮抓到的可复现内核 bug：从非 leader 线程调用 `exit_group` 时，进程被上报为「被
  SIGSEGV 杀死」而不是带着退出码正常退出。这很可能就是 `mpv` 那条「Lua 线程里 SIGSEGV」症状的来源
  （每个 Lua 脚本线程自己退出/收尾），当时认为值得优先修（`proc_exit_group()` 对 leader 的
  `proc_force_exit()` 与 `proc_exit(self)` 组合，见 `kernel/proc/exit.c:554`）。
- 试过并否掉的一个候选是 `proc_release_exiting_mm()`（`kernel/proc/exit.c:161`）：它在每个线程退出时
  都会 `arch_switch_addr_space_token(kernel_as)` + `mm_context_leave(t->mm, cpu)`，看起来像「兄弟线程还在跑
  就把本 CPU 从共享 mm 上摘掉」。按「只有 mm 最后一个引用才允许摘」加了
  `refcount_read(&mm->refcount) == 1` 守卫、重新编译内核并复测，v3 仍是 `sig=11`，java 仍是
  `255 0 255 0 255 0`，桌面无回归，该假设不成立，改动已 `git checkout` 撤回。这说明 leader 是真的发生了
  缺页（`kernel/proc/signal.c:848` 用 `-signal_wait_status_dumped(SIGSEGV, ...)` 收尾），而不是 active_cpus 记账问题。下一步应从
  「强制一个正在用户态自旋的线程退出」这条路径查（`proc_force_exit` → `exit_pending` → 调度器/trap
  边界消费），以及 leader 缺页时它的 mm 到底处于什么状态。
- v2 与 v3 的差别把范围缩得更小：v2（leader 自己调 `exit_group`，其他线程还活着）正常；v3（非 leader 调
  `exit_group`，于是 leader 被 `proc_force_exit()` 强制退出）报 SIGSEGV。触发点是「从非 leader 线程强制
  退出 group leader」，而不是「有活线程时退出 group」本身。
- 想再区分「被强制的线程处于用户态自旋 vs 阻塞在系统调用」而加的 v6/v7 变体在宿主上也是 sig=11，说明是
  探针自己写错了（宿主不该 SIGSEGV），这两个变体作废、该区分仍未被测到；不影响 v3 的有效性（v3 宿主
  code=42、客体 sig=11）。
- **更正：v3 本身也是探针的 bug，不是内核 bug**，上面 v3/v2 的结论一并作废。把内核日志里的
  `SIGSEGV: sepc=0x40135b stval=0x60030008, sp=0x60030000` 反汇编后发现：崩溃点是 `worker_group`
  线程启动后的第一条指令 `movq $0x0, 0x8(%rsp)`（初始化循环计数器），而 `sp=0x60030000` 恰好是该线程
  `mmap` 出来的栈区的上界，于是 `[rsp+8]` 落在映射区之外。也就是说探针把 `child_stack` 设成了 region
  的末尾（exclusive），`call fn()` 把返回地址压到 `rsp-8` 后，callee 的第一个局部变量又写回 `rsp+8`，
  正好越界。Linux 上"碰巧"没事，是因为相邻的匿名 `mmap` 往往首尾相接、越界那几字节落进了下一个映射；
  A20OS 不这么排布，于是正确地 fault。给 `child_stack` 留了 4KB 余量后，客体里 `v3` 5/5 次都是
  `code=42/sig0`，与宿主一致。所以 `exit_group`（含非 leader 触发、强制退出自旋中的 leader）在 A20OS 上
  是正确的，此前归因于它的分析撤回。这个教训值得记下：手写 clone 的 `child_stack` 不能落在映射末尾，
  且"宿主能跑"不等于"探针没问题"。
- 结论是内核的核心线程/内存/退出机制经逐一探测均正确。同一套「宿主+客体对跑」的无 libc 静态探针
  证明了：文件 `read()` 与 `mmap()` 数据完全一致（142MB jimage FNV 匹配 8/8）、并发线程不互踩各自
  缓冲（5/5）、`exit_group` 各形态（含非 leader 触发）退出码正确（5/5）、每线程 FS base/TLS 独立不被
  共享（每线程 `arch_prctl(ARCH_SET_FS)` 指向自己的块再循环读 `%fs:0`，4/4 次全 0 错配）。因此 `mpv`
  偶发崩溃与 `java` 退出码 255 不是由这些机制引起的；剩下可查的方向是 futex/信号等更细的语义，或它俩
  本就是程序层行为。对「视频可用」这个目标而言不受影响（ffplay 已验证可用，见下文 ffplay 条目）。
- v5 是编码分歧。A20OS 把负的 `exit_code` 当作信号死亡编码（`kernel/proc/wait.c:112-118` 的 `code < 0` 分支 →
  `(-code) & 0xFF`），而 Linux 对 `exit_group(-1)` 报的是正常退出 code=255。属于 ABI 语义差异，单独
  记录。
- v2 与宿主一致说明「主线程带活线程 `exit_group`」这条 JVM 路径本身没问题；结合前面
  `java.lang.Shutdown` 的证据，java 的 255 更可能是某个退出码/编码产物（例如内部以负值或 255 收尾），
  **不影响 JVM 运行**。
- 真正剩下待查的是「多线程进程的匿名内存偶发被写坏」。唯一还站得住的症状是 `mpv` 约 1/10 崩溃
  （LuaJIT GC 链表头变 NULL），单线程 LuaJIT 完全正常。方向应查线程生命周期（线程退出/reap、内核栈、
  per-thread trap 帧归属），而不是继续在文件 I/O 上找。建议下一步给内核线程退出路径加 trace/校验
  （`proc_exit`/`proc_force_exit`/`exit_pending` 与 `pending_exit_code` 的一致性），并用 `mpv --vo=null` ×N
  复现。

- **2026-09 补充**：同一个「链表头被写坏成 NULL」的签名，在一个完全无关的程序里又出现了一次。
  Minecraft 的死因链最后落在 `ld-musl-x86_64.so.1+0x4602b`，反汇编是
  `mov -0x10(%rdi),%rax; lea -0x10(%rdi),%rcx; cmp %rcx,0x10(%rax)`，一条双向链表自指针一致性检查；
  内核日志给出的 `stval=0x10` 说明当时 `rax == NULL`（另一轮 `stval=0x14c834000` 则是同一处 `rax`
  变成野值）。这与本条目上面 mpv 的 `libluajit+0x53320` 读 `*(G+0x130)` 得到 NULL 是同一个形状：一张
  本该有效的链表头被写成了 NULL/野值。两个互不相关的程序（LuaJIT、musl 的 ld.so）在同一内核上出现
  同一签名，让「多线程进程的匿名内存偶发被写坏」这条结论明显更站得住，而不是各自程序的 bug。

  「展开器 / DSO 查找逻辑本身坏了」这条同时被排除了：用宿主 g++ 编的无头文件 freestanding C++
  最小用例（`-nostdlib`；`__cxa_*`/`_Unwind_*`/`_ZTIi` 由客体 libstdc++/libgcc_s 在加载时解析），
  由已有的 `LD_PRELOAD` 垫片在第一次 `dlopen` 时驱动，覆盖 5 种情形（主线程捕获、主线程重抛+析构、
  主线程未捕获、非主线程捕获、非主线程未捕获），全部行为正确（42 / 14 / terminate+abort / 5 /
  terminate+abort），一次都没有出现 `ld-musl+0x4602b`。细节与日志见 `docs/distro/minecraft.md`。

- 现成的检测器有一个，但它是关着的，这是最值得先动的一步。`kernel/mm/slab.c` 里：
  - `BIG_CANARY 0xCAFEBABE`（第 58 行）只覆盖 big-alloc 块：块尾哨兵在 279 行写入，
    `big_alloc_canary_ok()`（61-66 行）校验，而且只在 `kfree` 时校验一次（377-382 行；命中就打
    `[SLAB BUG] kfree(...): big-alloc canary clobbered` 并 `panic`）。活着的对象被写坏要等它被释放
    才发现；一直不释放就永远发现不了。
  - `slab_validate_sp()`（230-257 行）是一个已经写好的 slab 页完整性校验器，检查的正是我们怀疑的
    这一类损坏：free_list 节点越界/未对齐、free_list 成环或溢出（`free_count > total`）、以及
    `in_use + free_count != total` 的计数不符；命中时还会把该页前 16 个 64 位字 dump 出来。
    但它被标了 `__attribute__((unused))`，两处调用点都被注释掉了：321 行（`kmalloc`，
    `"kmalloc-pre"`）与 468 行（`kfree`，`"kfree-pre"`）：
    `// slab_validate_sp(sp, "kmalloc-pre", obj_size);`

- 对上面那个检测器做了一遍代码审查，结论是可以放心打开：
  - 它遍历链表的方式是自保护的：循环体先把当前节点 `p` 校验为「在本页内且按 `obj_size` 对齐」，
    然后才在循环步进里读 `*(void **)p` 取下一个节点。即使链表已经被写坏，它也不会自己 fault，
    而是带着诊断 `panic`。这正是探针该有的行为。
  - 类型是安全的：`sp->total`/`sp->in_use` 是 `uint16_t`、`free_count` 是 `int`，
    `(int)(in_use + free_count) != total` 的整型提升没有溢出/回绕风险；`free_count > total`
    的成环判定也正确（对象数很小）。
  - 锁上下文两处都是对的（这点很关键，因为文件自己在 421-431 行强调「所有参与
    free-list/bitmap 状态机的检查都必须在 cache 锁内」）：`kmalloc` 的 321 行在
    `spin_lock_irqsave(&c->lock)`（288 行）之后，且成功路径上没有解锁；`kfree` 的 468 行同样在
    419 行加锁之后，两处都满足前提。
  - 它要抓的形状正是文件自己描述过的那个：421-431 行的注释写明「两个 CPU 同时释放同一对象会
    都看到 `alloc_bits` 已置位、于是把对象插入两次（通常形成自环），而这种损坏只会在之后的
    `kmalloc()` 里被发现」，这恰好对应 `slab_validate_sp` 的 `free_count > total`
    → `panic("slab_validate_sp: free_list cycle")`。
  - 代价是每次调用 O(空闲对象数)，在热路径上是可感知的开销，适合当诊断用，不宜长期默认开。

  建议的第一步很便宜，且已确认前提成立：把 321 与 468 两处调用打开（并去掉 `unused`），必要时在
  `kmalloc` 取到对象之后再校验一次；然后按本条目的复现方式（`mpv --vo=null` ×N，或多线程
  + 大量 `dlopen` 的压力负载）跑。它是为这一类损坏写好的现成探针，一旦命中就直接给出 `where` 和
  页面 dump，比继续做上游推理快得多。抓到人（或连续多轮全干净）之后，再决定要不要长期保留。

- **【已实测】**把上面那个检测器打开跑了压力负载：它没有命中，但同一轮把目标 bug 复现了出来
  （改动只用于诊断，跑完已经 `git checkout -- kernel/mm/slab.c` 还原，内核代码保持干净。）

  改动就是前面那三步：去掉 `slab_validate_sp` 的 `__attribute__((unused))`、打开 321（`kmalloc-pre`）
  与 468（`kfree-pre`）两处调用；`make ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both dev-build` 通过。
  然后注入压力负载：桌面起来后跑 40× `java -version` + 40× `mpv --vo=null`（都是短命、多线程的进程，
  专门制造线程/进程的创建-销毁 churn）。

  结果两边都很有信息量：

  - ✅ 负载确实复现了目标 bug，签名与上面记的逐字节一致：
    ```
    SIGSEGV: pid=631 code=13 sepc=0x638a6320 stval=0x8 abi=0
    FATAL:   pid=631 signal=11 abi=0 pc=0x638a6320 sp=0x77a3eb70 comm=lua/osc path=/extra/usr/bin/mpv
    vma_file=/extra/usr/lib/libluajit-5.1.so.2
    ```
    `comm=lua/osc`、`path=/usr/bin/mpv`、`libluajit-5.1.so.2`、`sepc=0x638a6320`、`stval=0x8`
    与第 69 行那条记录完全对上；40 次 mpv 里第 1 次就崩了（`rc=139`）。这套负载可以直接当
    可用的复现器，比原来「约 1/10」的描述好用得多。失败信息 `stval=0x8`、`pte=0x0 value=0x0`
    （该地址连页表项都没有）又是一次「读一个近 NULL 指针的小偏移」，与 `ld-musl+0x4602b` 那次的
    `stval=0x10` 是同一个形状。

  - ❌ slab 检测器全程没有命中（80 次迭代，无 `[SLAB BUG]`、无 `slab_validate_sp` panic）。这类损坏
    不是内核 slab 的 free-list 损坏。这和两个已知实例都是用户态结构这一点吻合：LuaJIT 的 `G`
    （`*(G+0x130)`）与 musl ld.so 的那张链表头，都在进程自己的地址空间里，内核的 slab 校验器根本
    看不到它们。检测方向应从「内核 slab」转到「多线程进程的匿名内存被写坏」，正好回到第 152 行
    的结论，只是现在多了两条硬证据：slab 侧已排除，以及两个 `NULL+小偏移` 的实例。

- **【已实测】**用户态匿名内存金丝雀：32 MiB 保护带在 40 个 mpv 进程里全程完好 ⇒ 不是「野写」
  也不是「整页被清零」

  既然两个实例（LuaJIT 的 `G`、musl ld.so 的链表头）都在进程自己的地址空间里，就把金丝雀直接放进
  出事的那个进程，而不是另写一个程序：给已有的 `LD_PRELOAD` 垫片加一段，`mmap` 32 MiB（8192 页）
  匿名内存，每个 64 位字填成 `0xCA9E000000000000 + i`（每个字唯一），再起一个线程每 100 ms 全量校验，
  发现不等于期望值就打印 `CANARY_MISMATCH`。然后用 `LD_PRELOAD=/usr/lib/libthrowtrace.so` 装进 mpv，
  跑 40 次 `mpv --vo=null`（沿用上面已复现的负载）。

  ```
  t=58515 tid=155 CANARY_START a=0x77840000 b=0x2000000     # 40/40 次都装上了
  t=58518 tid=155 CANARY_THREAD a=0x0                       # 校验线程 40/40 起成功
  ...(40 次)
  SIGSEGV: pid=158 code=13 sepc=0x638ab320 stval=0x8 abi=0  # 目标 bug 照旧复现
  FATAL:   pid=158 ... comm=lua/osc path=/extra/usr/bin/mpv
  ```

  - `CANARY_MISMATCH` = 0，`CANARY_BAD_PASS` = 0：32 MiB（8192 页）的保护带在每一个 mpv 进程里、
    包括崩掉的那一个，从安装到进程结束一个字节都没被改过。这排除了「野写 / 越界写碰巧落进匿名内存」：
    若是大范围乱写，32 MiB 的靶子几乎必然被打中；一次都没中，说明不是随机野写。同时也排除了
    「整页被清零 / 误回收」：8192 页中任何一页被清零都会被逐字校验抓到，而 0 次。
  - 合起来：损坏的粒度是亚页级、针对特定结构的，且结果常是把某个指针字段写成 0/NULL，正是两个实例的
    形状。这与两种机制吻合：use-after-free / 过早复用（结构被释放后又被别人分配并清零），或基于错误
    基址的存储（例如 TLS/FS base 错位，使写入落在一个「确定但错误」的地址上；这种写入恰好不会碰到
    我们那块保护带）。

  两条 harness 经验：
  1. `mpv` 完全不调用 `dlopen`（它的库都是链接期依赖），第一版把金丝雀挂在 `dlopen` 拦截器上，
     结果 `CANARY_START=0`、什么都没测到（假阴性！）。改挂到 `pthread_create` 拦截器
     （线程创建本就是这类损坏的相关活动）后 40/40 都装上了。
  2. 金丝雀的 `mmap(NULL)` 落点每次相同（`0x77840000`），做地址相关性对照时可用这一点。

- **【已实测】**加上「free 毒化」再跑同一负载：毒化确实生效，但崩溃签名一点没变、毒化值一次没出现

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

  - 毒化生效是确定的（235 条计数、第一条 `a=0x1 b=0x20`），所以这不是「探针没装上」的假阴性
    （第一版就是因为日志触发条件写成 `(g_frees & 0xFFFF)==0`、只在恰好 65536 次时打印，白跑了一轮）。
  - 签名没变、毒化值零次出现，于是排除「读一个仍然带毒化值的已释放块」。
  - 但有一个变体仍然成立，必须说清：若是 UAF，而那块内存在我毒化之后又被合法的新主人重新分配并
    清零，过期读者看到的正是 0/NULL，与观测到的 `stval=0x8` 完全一致。所以
    **「use-after-free + 复用后被清零」并未被排除**，被排除的只是「读到还带毒化值的空闲块」。
  - 真正的区分办法是隔离式分配器（quarantine）：`free` 后不把块还给分配器（毒化后扣住，设上限），
    这样过期引用永远读不到被清零的新内容，只会读到毒化值；同时可周期扫描扣住的块，直接抓出
    「往已释放内存里写」（比「读」更硬的证据，而且不必等崩溃）。这是下一步最该做的实验。

  这一轮还多抓到一个受害者：`tumblerd`（缩略图服务）也 SIGSEGV
  （`ADE/ALE: pid=123 sepc=0x432c6031 stval=0x8136a00e code=1`），与前面记过的 tumblerd 野指针
  是同一件事，再次说明这个损坏跨进程、跨程序。

- **【已实测，但本轮无结论】**隔离式分配器（quarantine）：扫描线程起来了 40/40，可每次扫描时隔离区都是空的

  做法：`free` 里先毒化、再不把块还给分配器（扣进隔离区，上限 65536 块/进程），另起线程周期性扫描
  隔离区；任何块不再等于毒化值就报 `FREE_WRITE`（= 往已释放内存里写，比「读」更硬的证据）。

  ```
  QUAR_THREAD = 40/40                    # 扫描线程每个 mpv 进程都起成功
  QUAR_SCAN   = 40，全部 a=0x1 b=0x0      # ← 每进程只跑 1 轮，且那一轮 g_qn == 0（隔离区空）
  FREE_WRITE  = 0
  SIGSEGV: pid=163 code=13 sepc=0x6396c320 stval=0x8    # 签名仍与基线一致
  毒化值出现次数：0
  ```

  - 线程起来了，但每进程只扫描 1 轮（`a=0x1`），而那一轮 `g_qn == 0`：唯一的扫描时刻隔离区是空的，
    等于什么都没检查。本轮既不能证明也不能排除「往已释放内存里写」。
  - 原因已看清：扫描线程在第一次 `free` 时才启动，启动后第 1 轮立即执行（此时还没扣下任何块），
    然后 `nanosleep` 200 ms，而 mpv 在第一次 `free` 之后活不到 200 ms 就退出了，于是永远只有第 1 轮。
    这是 harness 的时序问题，不是假设被否定。
  - 下一步的修法是让宿主进程活得远长于 200 ms：最简单是把负载换成 `mpv --vo=null --idle`
    （常驻，验证完再 kill），或者把金丝雀/隔离区放进长期存活的 java 进程（java 也是已确认的受害者：
    MC 那次崩溃就在 java 里）。这样隔离区能积累成千上万块、扫描能跑几百轮，才有机会抓到
    「往已释放内存里写」。
  - 顺带又踩了一次「编译失败却注入了旧 `.so`」：新代码里我写了 `u32`，而垫片只 typedef 了 `u64`，
    gcc 报 `unknown type name 'u32'`；但命令是 `gcc ... && echo` 串联的，报错后流程没停，
    于是把上一版的 `.so` 注进了镜像（现象：`QUAR_*` 标签一个都没有、签名跟上一轮逐字相同）。
    已经在流程里加了硬失败保护（`if ! gcc ...; then exit 1; fi`）：编译不过就绝不注入。
    教训是探针流水线必须让编译失败中止整条链，否则会拿旧产物跑出一份看似「阴性」的结果。

- **【已实测，有结论】**隔离区（quarantine）跑通：7910 个已释放块被扣住并扫描 ~150 轮，
  `FREE_WRITE` 为 0、签名仍是 `stval=0x8` ⇒ use-after-free 被排除（读与写两种形式都排除）

  把负载换成长期存活的宿主（`mpv --vo=null --idle`，每个活 25 秒、共 6 轮），这样扫描线程能跑够
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

  - 隔离区里 7910 个已释放块被扣住并毒化、被扫描 ~150 轮（全轮 621 次扫描），覆盖面足够，不是「没测到」。
  - `FREE_WRITE` = 0，即没有「往已释放内存里写」。
  - 签名仍是 `stval=0x8`、毒化值一次未出现，那个 NULL 不是从已释放（且永不复用）的块里读出来的：
    若是，隔离区会让它读到毒化值，而不是 0。
  - 于是 use-after-free 被排除（读、写两种形式）。这同时关掉了前面「UAF + 复用后被清零」那个
    仍然成立的变体，因为隔离区根本不复用。

  到这里的排除清单已经相当长：内核 slab free-list、pbuf 双重释放、通用展开/DSO 查找逻辑、
  线程创建失败、音频后端、dlopen 卡住、匿名内存野写、整页清零/误回收、use-after-free。
  剩下的候选集中在两处：

  1. 基于错误基址的存储（例如 TLS/FS base 错位，写入落在一个「确定但错误」的地址，且写进去的多半是
     0 或小值，正好不会碰到大范围金丝雀，也不是复用已释放块）；
  2. 内核在某条路径上「返回 0 / 交出一张零页」（在页级或 syscall 返回值层面，而不是 malloc 复用）。

  下一批实验应针对这两条。

- **【代码级发现，候选机制】**`clear_child_tid` 的清理是「绕过权限/COW 的裸物理写」，且用户指针毫无校验

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

  与 Linux 语义有两处实质差异：

  1. 绕过了所有权限 / COW 检查。Linux 在这里用带访问检查的 `put_user(0, tidptr)`
     （不可写 / 未映射会安全失败）；这里是「翻译出物理帧 → 经内核线性映射直接写」，
     不检查该页此刻是否可写、是否 COW 共享、是否只读文件页。
     - 若该页正与别的任务 COW 共享，这一写会把对方那份也改掉；
     - 若指向只读文件页，还会改到页缓存，影响所有映射它的进程。
  2. 用户指针完全没有范围 / 有效性校验。`sys_set_tid_address()`
     （`kernel/abi/linux/sys_proc.c:180-184`）只是 `t->clear_child_tid = tidptr;`，
     这与 Linux 一致（Linux 也不校验指针）；差别在于 Linux 的写是受检查的、这里的是裸物理写。

  值得优先验证的原因是它与前面所有排除项都对得上：

  - 写进去的是 0，正好是「某个指针字段变成 NULL」那个签名；
  - 触发点是任务退出（`proc_exit`），与「线程/进程创建-销毁 churn」这个已知相关性完全吻合；
  - 是亚页级、精确地址的 4 字节写，会被 32 MiB 金丝雀漏掉（而金丝雀已证明不是野写），
    也不是整页清零、更不是 use-after-free（这三条都已实验排除）；
  - 若落在 COW 共享帧或页缓存上，就是跨进程污染，与「tumblerd / mpv / java 都中招」吻合。

  验证方案两条任一即可给结论：

  （a）先只加探测、不改行为：在 `*kv = 0;` 之前判断目标是否「本任务私有且可写」（或查该物理帧的
      引用计数 / 是否 COW 共享），不是就打一条 `[CTID]` 日志；用已经可复现的 mpv 负载
      （40 次里约 1 次崩）跑几轮，看这条日志是否出现。
  （b）直接改成受检查的用户写（等价于 Linux 的 `put_user(0, ctid)`：检查映射存在且可写，
      必要时走 COW 破坏流程），再跑同一负载，看 mpv / tumblerd 的崩溃是否消失。因果性最强的检验。

  保留意见（不要过度断言）：这条能解释用户态那个「指针字段变 NULL」的签名，但解释不了内核侧
  「pbuf 里出现用户态地址」那一幕（那需要「写入一个用户态指针」，而这里是写 0）。两条线可能仍需
  分开收尾；本条应作为候选机制去验证，而不是当作已定的根因。

- **【根因已定位，并有确定性最小复现】**就是 `clear_child_tid` 那条「裸物理写」缺了 COW 破坏，
  跨进程内存损坏的原因找到了

  用一个纯用户态、最小、确定性的复现器证实了上面那条候选机制。复现器逻辑：

  1. 父进程把全局变量写成 `0x12345678`，然后 `fork()`；
  2. 子进程不写它（于是该页与父进程 COW 共享），把 `set_tid_address()` 指向它，然后 `exit()`；
  3. 父进程 `wait4()` 后检查自己那份还是不是 `0x12345678`。

  正确实现（Linux 用受检查的 `put_user(0, tidptr)`）会在写之前破坏 COW、给子进程一张私有副本，
  父进程那份保持原值；而 A20OS 是「翻译出物理帧 → 经 `pfn_to_virt` 直接写该帧」，于是父子共用的
  那一帧被写成 0，父进程的变量被改成 0。

  同一个二进制在宿主与客体对跑（这是本项目一贯的「宿主参照」方法）：

  ```
  宿主（参照，Linux）： test 12345678 clean     control 0badf00d clean    # 5/5 轮全 clean
  客体（A20OS）：      test 00000000 POLLUTED  control 0badf00d clean    # 20/20 轮全 POLLUTED
  ```

  - `test` 轮（目标页与父进程 COW 共享）：客体 20/20 全部被写成 0，宿主 5/5 完好。
  - `control` 轮（子进程先写该页、真正破坏 COW 变成私有）：客体也是 clean ✓
    ⇒ 这证明失效点精确地是「没有做 COW 破坏」这一环，而不是别的东西。
  - 复现率 20/20（确定性）、宿主全对 ⇒ 这是 A20OS 的 bug，不是探针问题。
  - 复现器：`/tmp/opencode/ctidtest.c`，构建
    `gcc -static -nostdlib -ffreestanding -fno-builtin -no-pie -o ctidtest ctidtest.c`
    （静态、无 libc，宿主与客体都能跑）；客体里放在 `/usr/bin/ctidtest`。

  它解释此前全部观测，也与所有排除项相容：

  - 写进去的是 0，正是「某个指针字段变成 NULL」那个签名；
  - 触发点是任务退出（`proc_exit`），与「线程/进程创建-销毁 churn」相关性吻合；
  - COW 共享的那一帧同时属于另一个任务，就是跨进程污染（mpv / tumblerd / java 都中招）；
  - 精确地址的 4 字节写是亚页级，不会被 32 MiB 金丝雀打到（金丝雀已证明不是野写）；
  - 既不是野写、也不是整页清零、更不是 use-after-free（这三条均已实验排除）；
  - 与内核 slab、pbuf 双重释放、展开逻辑、线程创建失败均无关（均已排除）。

  本条长期悬而未决的「多线程进程的匿名内存偶发被写坏」到此定位到具体代码与具体语义缺陷：
  `kernel/proc/exit.c` 的 `proc_clear_child_tid_direct()` 用裸物理帧写替代了受检查的用户写，
  缺少 COW 破坏与权限检查。

  修法是把这条裸写换成受检查的用户写，等价于 Linux 的 `put_user(0, ctid)`：若目标页是 COW（或只读），
  先按正常流程破坏 COW、拿到本任务私有副本，再写 0；不可写/未映射时安全失败（像 Linux 一样直接返回）。

  修复与验证（均为已实测）：

  - 修法是把 `proc_clear_child_tid_direct()` 里的裸物理写换成受检查的用户写
    `copy_to_user(ctid, &zero, sizeof(zero))`。核心里这条路径本来就会做该做的事：
    `user_resolve_leaf(..., write=1, ...)`（`kernel/mm/mm.c:755-779`）在叶子不存在时走
    `handle_demand_fault()`、在存在但不可写时调用 `handle_cow_fault()` 破坏 COW，
    再重读 PTE，最后对未映射/不可写的地址安全返回 `-EFAULT`，即 Linux `put_user(0, tidptr)` 的语义。
    出问题的那个函数绕过了它，自己 `pt_translate + pfn_to_virt` 直接写帧，跳过了 COW 破坏。
  - 验证用同一个复现器、同一个镜像，只换内核：
    ```
    修复前：test 00000000 POLLUTED   control 0badf00d clean   # 20/20 与 20/20
    修复后：test 12345678 clean      control 0badf00d clean   # 20/20 与 20/20  ← 与宿主逐项一致
    ```
    修复后 `test` 轮 20/20 的污染全部消失，且与宿主（Linux）结果一致。
  - 它之所以偶发，是因为只有当任务的 `clear_child_tid` 所指的页此刻与另一个任务 COW 共享时才发作，
    所以表现为「约 1/10」；一旦发作就是把另一个任务（通常是 fork 出来的父子进程）的 4 字节字段清零，
    于是症状永远是「某个指针字段变成 NULL」。这与 mpv / tumblerd / JVM 那几条症状完全对得上。

- **【重要更正】**cleartid 这个缺陷是真的、也真的修好了，但它不是 mpv/tumblerd 那几条崩溃的原因

  修完之后，用同一套会复现症状的负载（这次加大到 60× `java -version` + 120× `mpv --vo=null`）
  在修复后的内核上再跑一遍，结果症状照旧出现：

  ```
  VF java done=60   VF mpv done=120                      # 负载确实跑完了
  SIGSEGV: pid=869 code=13 sepc=0x638a6320 stval=0x8     # 与修复前同样的签名
  FATAL:   pid=869 ... comm=lua/osc path=/extra/usr/bin/mpv
  ADE/ALE: pid=123 sepc=0x45bb6031 stval=0x8136a00e code=1
  FATAL:   pid=123 ... comm=tumblerd                     # tumblerd 也照旧崩
  ```

  **必须更正上面那句「完全对得上」**：

  - `clear_child_tid` 的缺陷本身是真的、修复也是对的（`ctidtest`：修复前 20/20 污染、
    修复后 20/20 干净，宿主对照 5/5 全对），它确实是一个独立的、确定性的跨进程内存损坏 bug，
    修复应当保留；
  - 但它不是 mpv / LuaJIT / tumblerd 这些偶发崩溃的原因：修复后这些症状依然复现，
    且签名逐字相同（`comm=lua/osc`、`sepc=0x638a6320`、`stval=0x8`）。
    此前仅凭「形状一致」就把它认定为那些症状的根因，属于过度推断，在此撤回；
  - 同理，MC 那次 `ld-musl+0x4602b` 崩溃没有在修复后复测过，所以对它同样不能下这个结论；
  - 「多线程进程的匿名内存偶发被写坏」这条仍未解决（只是排除项又多了一条：cleartid 的 COW 缺陷已被
    证明是另一回事）。**mpv 崩溃的根因仍然未知。**

- **【同类缺陷：审计发现第二处】**`futex` 的 PI 路径也在「自己翻译地址、裸写物理帧」，而且写的也是 0

  在排查 mpv 那条崩溃时，把「自己 `pt_translate` 出物理帧再直接写」这个已被证明有害的写法在核心里
  过了一遍，发现第二处同类代码：

  - `kernel/ipc/futex.c` 的 `futex_user_word_map()`（541-564 行）`pt_translate` → `pfn_to_virt` 后
    返回一个指向内核线性映射的 `volatile int *word`，调用方拿它做原子写：
    ```c
    volatile int *word = NULL;
    futex_user_word_map(t, uaddr, &word, &pkey);      // 581 / 654 行
    ...
    __atomic_store_n(word, zero, __ATOMIC_RELEASE);   // 668 行：同样是裸帧写，写的也是 0
    ```
    与 cleartid 那条完全同类：若目标页此刻与别的任务 COW 共享，这一写会把对方那份也改成 0。
  - 已核对为「正确」的一处是 `exit_robust_list()`（680-713 行），它用的是 `copy_from_user()` /
    `copy_to_user()` ✓，说明核心里正确写法是有的，出问题的两处（cleartid 与这里）都是自己手写。
  - 也顺带澄清语义边界：对 MAP_SHARED 页裸写帧是对的（大家都映射同一帧，本就该一起改），
    错的只是 COW-private 的情况；`user_resolve_leaf()` 两种情形都能正确处理 ✓。

  这次记录为待修、而不当场改，有两个原因：

  - 触发面很窄：这条只在 PI（优先级继承）/ robust futex 路径上写；而 mpv / LuaJIT /
    tumblerd / JVM 用的是普通 futex（`FUTEX_WAIT/WAKE`，走的是只读的 `futex_user_load_locked()`），
    它大概率解释不了那几条偶发崩溃，这也是为什么先不动它。
  - 修复有死锁风险：`futex_user_word_map()` 的调用点在 `mm->lock` 之内（该函数注释明确写了
    “Caller must hold mm->lock”），而正确的 COW 破坏要调 `handle_cow_fault()`
    （声明在 `kernel/include/mm/fault.h:11`）；若它内部也取 `mm->lock`，原地调用就会自死锁。
    因此修法必须把 COW 破坏挪到取 `mm->lock` 之前（或另设无锁路径）；也不能照搬 cleartid 那招，
    这里要的是原子写，而 `copy_to_user()` 是 memcpy，不适用。

  记为已定位、待修的同类缺陷，修法要点如上。

- **【已实测，阴性】**COW 破坏本身是保数据的：宿主/客体对跑 20/20 都干净 ⇒ 这条假设排除

  动机是此前测试的一个盲点：32 MiB 金丝雀只读不写，所以它从未触发过 COW 破坏，也就是说
  「COW 破坏是否正确」这一路此前完全没测过。而 mpv 会 `fork()`（ytdl/osc 脚本），fork 之后父进程
  再写那些继承来的页就必须触发 COW 破坏；若破坏时拷错（或拷成零页），父进程的页就会变成 0，
  指针字段变 NULL，与签名吻合。于是补了这个确定性最小复现器：

  - 父进程把整页填成 `i ^ 0x5A` 的图案 → `fork()` → 子进程睡 300ms 保持存活（让页面仍与父
    COW 共享）→ 父进程只写 1 个字节（必然触发 COW 破坏）→ 校验其余 4095 字节是否原样；
  - `control` 轮：同样填+写但不 fork（无 COW）⇒ 任何实现都应干净，用于证明探针本身没问题。

  实测：

  ```
  宿主（参照）：cow 00000000 clean   control 00000000 clean    # 5/5 全干净
  客体（A20OS）：cow 00000000 clean   control 00000000 clean    # 20/20 全干净
  ```

  「COW 破坏会损坏数据」这条假设被排除（`bad=0`，与宿主逐项一致）。复现器是
  `/tmp/opencode/cowtest.c`，同样是静态、无 libc、宿主客体都能跑的 freestanding 程序。

- **【已实测，阳性】**PI futex 那条同类缺陷被证实：`pi 00000001 POLLUTED` 20/20（宿主 5/5 干净）

  用与 cleartid 完全相同的「确定性最小复现 + 宿主对照」方法，证实了审计发现的那第二处
  同类缺陷（这次是先证实、再下结论）：

  - 父进程把整页填成 `i ^ 0x5A` 图案，把页内偏移 64 处的 futex word 置 0（未锁），然后 `fork()`；
  - 子进程 `futex(FUTEX_LOCK_PI)`（PI 锁会把 owner TID 写进该 word），随后持锁退出（不 unlock）；
  - 父进程 `wait4()` 后检查自己那一份：word 应仍为 0、整页图案应原样。

  正确实现会在写之前破坏 COW（写落到子进程的私有副本）⇒ 父进程不受影响；而 A20OS 的
  `futex_user_word_map()` 返回的是裸帧指针、`__atomic_store_n()` 直接写它 ⇒ 写进两者共享的那一帧
  ⇒ 父进程的 word 被改写。

  ```
  宿主（参照）：pi 00000000 clean      pi-control 00000000 clean   # 5/5 全干净
  客体（A20OS）：pi 00000001 POLLUTED   pi-control 00000000 clean   # 20/20 全 POLLUTED
  ```

  - `bad=1` 正是「word 不再是 0」这一项坏掉（图案本身完好）⇒ 父进程的 futex word 被跨进程改写；
  - `pi-control` 轮（子进程先脏了该页、COW 已由子进程自己破坏）客体也是 clean ✓
    ⇒ 与 cleartid 一样，失效点精确地是「内核没有为这次写破坏 COW」；
  - 复现率 20/20（确定性）、宿主全对 ⇒ 第二个真实的跨进程内存损坏 bug，已证实。
  - 复现器：`/tmp/opencode/pitest.c`（静态、无 libc、宿主客体都能跑）。

  这确认了上一条的审计推断是对的。但它仍未修复：修法必须「在取 `mm->lock` 之前完成 COW 破坏」
  （原地调用 `handle_cow_fault()` 会自死锁），且此处需要原子写，不能用 `copy_to_user()` 替代。

  修复方案的插入点已找好，供下一步实施：

  - 调用点是 `kernel/abi/linux/sys_futex.c:49` / `:51` / `:63` 三处 `futex_pi_acquire()`
    （`FUTEX_LOCK_PI` / `FUTEX_TRYLOCK_PI` 及相邻分支），它们都在引擎取 `mm->lock` 之前，
    正是可以安全破坏 COW 的位置；
  - 核心里没有现成的「为写而破坏 COW」独立 helper（`user_resolve_leaf()` 里那段是 `static`），
    所以需要把那段逻辑（叶子不存在 → `handle_demand_fault()`；存在但不可写 → `handle_cow_fault()`）
    提炼成一个可导出的 helper，在这几处先调它、再进引擎；
  - 做完之后，`futex_user_word_map()` 返回裸帧指针才是安全的（此时该页已是本任务私有），
    并且仍需保留原子写（不能换成 `copy_to_user()`）。
  - 验证方式是同一个 `pitest` 复现器：修复后客体应当变成 `pi 00000000 clean`，与宿主一致
    （就像 cleartid 那次 `20/20 POLLUTED → 20/20 clean` 一样）。

  **✅ 已按上述方案修复并验证**：

  - 实现是在 `kernel/mm/mm.c` 新增可导出的 `user_prepare_write(task_t*, uint64_t)`，把
    `user_resolve_leaf()` 里那段「叶子不存在 → `handle_demand_fault()`；存在但不可写 →
    `handle_cow_fault()`；最后确认可写」的逻辑独立出来，并在 `kernel/include/sys/usercopy.h` 声明；
    然后在 `kernel/ipc/futex.c` 的 `futex_pi_acquire()` 循环顶部与 `futex_pi_release()` 取锁之前
    各调用一次，即在取 `mm->lock` 之前完成 COW 破坏，之后 `futex_user_word_map()` 返回的裸帧指针
    才是安全的。放在引擎里（而不是 Linux ABI 的 `sys_futex.c`）可同时覆盖 Native ABI。
  - 编译踩坑：`usercopy.h` 原来不认识 `task_t`，原型里的 `struct task_t *` 被当成「在参数表里声明
    新 struct」，`-Werror` 直接报错；在头里补一句 `struct task_t;` 前置声明即可。
  - 验证用同一个 `pitest`、同一个镜像，只换内核：
    ```
    修复前：pi 00000001 POLLUTED   pi-control 00000000 clean   # 20/20 与 20/20
    修复后：pi 00000000 clean      pi-control 00000000 clean   # 20/20 与 20/20 ← 与宿主逐项一致
    ```
    20/20 的跨进程污染全部消失，与宿主（Linux，5/5 clean）一致。
  - 残留：`user_prepare_write()` 是在取锁之前破坏 COW，因此「prepare 之后、
    锁内存储之前」若恰好发生 `fork()` 使该页重新变成 COW，理论上仍有极窄的竞态窗口。
    要彻底关闭它，需在 `mm->lock` 内复查 PTE 可写性（不可写则解锁重试）；本轮未实施。

- **【第三处 + 第四处同类缺陷】**信号投递路径也在「拿裸帧重新映射」；另有影子栈 token 一处

  沿同一 bug 类继续审计 `pt_translate` / `pfn_to_virt` 的用法，又找到两处：

  - `kernel/proc/signal.c` 的 `signal_make_page_exec()`（28-54 行；唯一调用点 926 行）是
    信号投递路径：为了让栈上的 sigreturn trampoline 可执行，它 `pt_translate` 取出物理帧，
    然后 `pt_unmap()` + `pt_map(同一个 pa, ...writable_dirty_exec...)`，即把同一个帧重新映射成
    可写可执行。要注意信号帧本身是用 `copy_to_user()` 写的（915/918 行）✓，所以帧写是安全的；
    危险的是这个重映射：若该页此刻仍与另一个任务 COW 共享，这一步会让本任务拿到
    共享帧的可写别名，此后本任务的任何写入都会改到对方那一份 ✗。
  - `kernel/abi/linux/sys_missing.c:563-571`（CET 影子栈 token）：`handle_demand_fault` 之后
    `pt_translate` + `pfn_to_virt` 直接 `*tok = ...`，同样是裸帧写（影子栈少见，但同类）。

  信号这处最可疑的原因是：`signal_make_page_exec()` 在每次投递信号时都会执行，而它操作的页就是
  用户栈；`fork()` 之后父进程的栈页与子进程 COW 共享，而 mpv（ytdl 助手）、tumblerd（GLib 起助手）
  都会 fork 子进程、并在子进程退出时收到 SIGCHLD，「信号投递 + 刚 fork 过的栈页」正好凑齐。
  这比 futex PI 那条（触发面很窄）更有可能解释那几条偶发崩溃。
  谨慎起见：本轮做的是代码定位与修复，症状层面的因果尚未用端到端方式证实，故措辞按「可疑」处理。

  修法（本轮已实施）是在 `signal_make_page_exec()` 的 `pt_translate` 之前调用
  `user_prepare_write(t, page)`，先确保该页是本任务私有（必要时破坏 COW），
  之后的 unmap/map 操作的是私有帧，不会再产生共享别名。与 cleartid / futex 两处同一修法。

  **⚠️ 更正（同一轮验证的结果）**：修好之后，用同一套症状负载（60× `java -version` +
  120× `mpv --vo=null`）在修复后的内核上再跑一遍，症状依旧复现：

  ```
  SIGSEGV: pid=925 code=13 sepc=0x638a6320 stval=0x8
  FATAL:   pid=925 signal=11 abi=0 pc=0x638a6320 sp=0x77dbbc40 comm=lua/console path=/extra/usr/bin/mpv
  ```

  上面那句「更有可能解释那几条偶发崩溃」是过度推断，在此撤回：信号这一处确实是同类缺陷、也确实该修
  （已修），但它不是那些偶发崩溃的原因。

  - 同一轮的回归探针全部干净：`ctidtest` 5/5、`cowtest` 5/5、`pitest` 5/5，
    说明这一轮内核改动没有破坏 COW / cleartid / futex 这几条路径 ✓。
  - 至此同类缺陷共找到 3 处、修了 3 处（cleartid、futex PI、signal），
    但 mpv / LuaJIT / tumblerd 那几条症状仍未解释。

**【根因已定位并修复：匿名 fault-around 窗口丢了 VMA 就把 fault 判死，多线程必崩；窗口还可能把零页装进空洞】**

  这一条把上面那条长期悬置的「多线程进程的匿名内存偶发被写坏」收尾了。定位靠的是一个**全新的、
  确定性的最小复现器**，而不是此前一直用的 mpv 症状负载。

  复现器是 `user/cmds/stress/mtcorrupt_test.c`，门禁 `smoke-mtcorrupt`
  （`NR_CPUS=4`、`CONFIG_SLAB_DEBUG=1`）。它分两段：

  1. 若干条带 `PROT_NONE` 保护页的记录（数据 4096 B + 守卫页），每个字填成唯一的非零图案，
     外加一张**所有线程共享的双向链表**，线程对链表做 push/pop 并校验自指针、magic 与无环；
  2. 在同样的 4 个 worker 持续 mmap/munmap、堆 churn、读写记录的同时，疯狂创建/回收短命线程
     （`CHURN_ROUNDS*10` 个 `churn_thread`）。

  关键性质：**单 CPU 必过，4 CPU 每次都崩**。这正是此前整张门禁矩阵漏掉它的原因 ——
  现有所有共享 `mm_struct` 的门禁都跑在 `-smp 1` 上，而缺陷只在真并发下才成立。
  崩溃签名与本条目里记的 mpv / MC 完全同形：`SIGSEGV ... code=13`（store fault）、
  随后 `FATAL: signal=11`，`stval` 落在一个**完全合法、RW、可写的匿名 VMA 内部**。

  根因是 `kernel/mm/fault.c` 的匿名 fault-around 窗口（`handle_demand_fault_locked()` 内
  `ANON_FAULT_AROUND_PAGES = 4`）。它为了在一次 fault 里装 4 页，会**主动放开 `mm->lock`** 去分配帧
  （`vma_get(vma); spin_unlock(&t->mm->lock);`），拿回锁后做一次校验：

  ```c
  } else if (mm_find_vma(t->mm, page_va) != vma) {
      ... 归还所有帧 ...
      return -1;                     /* ← 就是这里 */
  }
  ```

  指针比较失败被当成了错误。但**这不是错误**：`mm_insert_vma()` 在合并相邻匿名 VMA 时保留的是
  **新对象**、把旧对象 `mm_vma_defer()` 掉，所以一个谁都没碰过的映射，会在别的线程一次 `mmap`
  之后**换掉身份**。`vma_can_merge()` 要求 `vm_flags` 与 `pte_flags` 全等，所以新对象在语义上是
  等价的：地址照样映射、照样可写。把这个窗口判成 `-1`，等于在一次完全良性的链表合并上杀进程。

  诊断过程中有两处弯路值得记下来，避免下次重复：

  - 曾怀疑过缓存的 VMA 二分索引（`mm->vma_index[]`）是唯一原因，于是做了「强制线性扫描」的对照构建，
    结果照样崩 —— 索引不是原因。真凶只在**放开锁的那段窗口**里，而线性扫描与二分索引都能正确找到
    当前 VMA，两者的差别只在于窗口返回时指针是否还等于当初那个。
  - `kerr` 的输出在多 CPU 下会**交错撕裂**（日志里出现过
    `win: enter va=0x91cc2000 end=0x91cc6000 vma=[[ERR0x91cc2000] ,0x91ce2000)` 这种行内嵌行）。
    当时把撕裂读成了「`vma->end` 是垃圾值 `0x91cded9020000`」，差点得出「VMA 结构被写坏」的结论。
    **多 CPU 下不要用 `kerr` 的自由格式输出去做结构完整性判断**；要么落进环形缓冲，要么改用锁保护。

  修复分三处，都在 `kernel/mm/fault.c`：

  1. **重试而不是判死**：新增 `MM_FAULT_RETRY`（`-EAGAIN`）与 `MM_FAULT_RETRY_MAX = 8`，
     窗口发现 VMA 身份变了就归还帧、返回 `MM_FAULT_RETRY`；入口 `handle_demand_fault_access()`
     改成循环调用新的 `handle_demand_fault_attempt()`，重试即重新在锁内解析当前 VMA ——
     正是窗口放开锁时做不到的那一步。重试次数有界，防止病态合并把 CPU 烧在这上面。
  2. **窗口按当前 VMA 重新夹紧**：窗口跨度 `end` 是在**放开锁之前**按当时的 `vma->end` 算的。
     而并发 `munmap`、以及 `mprotect` 触发的 `vma_split`/`mm_split_vma_at` 都是**就地**把
     `vma->end` 调小，不换对象。所以拿回锁后必须再夹一次（`if (end > vma->end) end = vma->end;`）。
     不夹的后果更隐蔽：装进去的那些页落在**空洞**里，PTE 是「present + 全零」。fault 报告成功，
     没有任何线程写过那块内存，而下一次 `mmap` 到同一地址会**继承**这些 PTE、当成应用自己
     fault 进来的页 —— 这正是本条目记的那个「指针字段变 0/野值」的形状，而且是**静默**的，
     比直接 SIGSEGV 更难查。超出新 `end` 的帧由 `fault_map_window()` 的短计数自动归还。
  3. `handle_demand_fault_attempt()` 的收尾不再对每个非零返回打列表 dump。

  顺带修掉了同一处的一个真实缺陷：`vma_split()` 是 VMA 链表 mutator，但原来**只接收 VMA**、
  不动 `mm->vma_index[]`。`mm->vma_split` 全树唯一的调用者 `mprotect` 是唯一一个不做失效的 mutator，
  于是索引数组变成一张「指针快照」：后面每个槽位都错一位，而数组仍按 `start` 有序，二分搜索照样收敛、
  不报错，只是收敛到一个不覆盖该地址的 VMA 或干脆收敛到空。签名改成 `vma_split(mm, vma, split)`，
  在函数内部做 `MM_VMA_INDEX_MUTATION` 失效。

  实测（同一镜像，只换内核，`-smp 4`）：

  ```
  修复前：SIGSEGV code=13 ... FATAL: signal=11      # 每次必崩
  修复后：MTCORRUPT: phase 1/2 done
          MTCORRUPT: phase 2/2 done
          MTCORRUPT: PASS                           # 连续 6/6 轮
  ```

  门禁同时把 `SIGSEGV` / `SLAB DEBUG` / `FATAL` 列为 `forbid`，所以这条以后会在回归里被守住。

  与本条目已有结论的关系：三处「裸帧写」（cleartid、futex PI、signal）已修且保留，它们都是**真的**，
  但如上所述都不解释这些偶发崩溃；本条是**另一条独立的、且是 SMP 才有的**机制。至此
  「多线程进程的匿名内存偶发被写坏」有了具体代码、具体语义缺陷和确定性门禁。

**【MC 的 OpenAL 有结论了：不是内核 bug，而是「把 glibc 版原生库塞进 musl 客体」】**

  用和 `ld-musl+0x4602b` 相同的办法（把 `__cxa_throw` 的调用方地址减去它所在 mapping 的基址），
  拿到了抛出点的稳定偏移 `libopenal.so + 0xf168`（多轮都落在这个偏移上，低位一直是 `...168`）。
  顺着这条线查下去，根因清楚了：

  - 抛异常的 `libopenal.so` 是 LWJGL 自带的 natives（客体里
    `/usr/share/a20-media/1.21.11/natives/libopenal.so`，运行时被 LWJGL 解到
    `/tmp/lwjgl_root/3.3.3+5/x64/`）。把它从镜像里抠出来看 `NEEDED`：
    ```
    libdl.so.2  libstdc++.so.6  libm.so.6  libgcc_s.so.1  libpthread.so.0  libc.so.6
    ld-linux-x86-64.so.2
    ```
    这是一个 glibc 构建的库（`ld-linux-x86-64.so.2` / `libc.so.6`），不是 musl 的 ✗；
    而 `libstdc++.so.6` 出现在 `NEEDED` 里 ⇒ 抛 `std::system_error` 的 C++ 代码就在这个库内部 ✓。
  - 它能被加载起来，是因为客体装了 Alpine 的 gcompat（`/lib/libgcompat.so.0`，以及
    `/lib/ld-linux-x86-64.so.2`、`libc.so.6`、`libpthread.so.0`、`libdl.so.2`、`libm.so.6` 这些
    glibc ABI 的桩）；这个库一共从 glibc ABI 导入 283 个符号。
  - 结论是 MC 的 OpenAL 失败属于「glibc 版原生库 + gcompat 兼容层」这条链，而不是 A20OS 内核缺陷。
    `std::system_error` 正是 C++ 在 `pthread`/`mutex`/`std::thread` 一类调用拿到错误码后抛出的典型形态，
    而这类调用在 gcompat 上最易失真。
  - 可行动方向是改用 musl 构建的 OpenAL / LWJGL natives（或让 LWJGL 指向 musl natives），
    或补齐 gcompat 在这一路上的语义，不必再往内核里找。

  附注：`+0xf168` 落在该库的第一个 LOAD 段内，且该库已 strip；要继续点名具体函数，
  需要用它的 `.eh_frame` 或带符号构建，但对「glibc/gcompat」这个结论已非必要。
- 影响：JVM 可用，所以 Minecraft 的第一障碍其实是 GL 链（IN_FORMATS → PRIME → GL 渲染器）；
  mpv 那条 1/10 的多线程内存问题，其内核侧机制已在本节「根因已定位并修复」一条里定位并修好
  （匿名 fault-around 窗口的 VMA 身份竞争），并由 `smoke-mtcorrupt`（`-smp 4`）守住；
  症状层面的 mpv 端到端复测尚未重跑。
- 2026-09 更新（信号对齐 + ucontext 布局修掉后）：`68abf68f`（handler 入口对齐）之后 JVM 能跑 handler、
  能打印崩溃报告；暴露出的真正崩溃是 HotSpot 的隐式空指针检查（`SHA5.implCompress0` 的数组访问依赖
  「空数组 fault → 处理器抛 NPE」，A20OS 上没被识别 → JVM 当致命崩溃）。最小复现器（宿主 javac 编 `.class`
  注入客体）显示 `o.hashCode()` 能正常抛 NPE、`arr[0]` 直接把 JVM 打崩。用
  `-XX:+UnlockDiagnosticVMOptions -XX:-ImplicitNullChecks` 可规避（`java -cp client.jar Main` 不再崩，
  只在缺失的 `joptsimple` 上报 `NoClassDefFoundError`）。
- ucontext/siginfo ABI 均已修：A20OS 的 ucontext 与 Linux/musl 不一致（`uc_sigmask` 排在
  `uc_mcontext` 前、mcontext 私有寄存器序、FPU 内嵌而非 `fpregs` 指针），已按 musl 对齐
  （`_Static_assert` 钉死偏移与尺寸；`2c1d0dbe`）；同步 SIGSEGV 的 siginfo 也改成 Linux 的
  `SEGV_MAPERR` + 故障地址（`4cf43e37`）。修完后 `bad uc->uc_mcontext.fpregs` 与错误报告自身 fault 都消失。
- 启动器两个 classpath bug（已修 `f3469887`）：`classpath.txt` 是单行冒号分隔，启动器的贪婪
  `sed '^.*/libraries/'` 把整条 classpath 塌成一条，又把 client.jar 粘在无结尾冒号的 CP 后面，这正是
  最初那个 `ClassNotFoundException` 的来源；并加上 `-Dos.name=Linux`（LWJGL/Minecraft 拒绝平台名 "A20OS"）。
- 现状是启动器能跑完整套真实启动流程、成功加载 LWJGL 3.3.3+5 并开窗成功。LWJGL 自带的
  `libjemalloc.so` 会 fault（`-Dorg.lwjgl.system.allocator=system` 绕过）；开窗时 X11 曾报
  `XIO: fatal IO error 90 (Message too large)`，根因是 A20OS 的 AF_UNIX 旧队列路径把单条消息卡在
  `NET_MAX_PAYLOAD`(64KiB)，**已修（`4e2aeb9f`）**：STREAM 大写按 64KiB 分片。现在卡在 DNS
  （`UnknownHostException: api.minecraftservices.com`，客体未配 resolver）。
  完整过程与证据见 [minecraft.md](minecraft.md)。

### x86_64 可用 RAM 曾硬编码成 1 GiB（已修复：改从 multiboot 内存图取）

`kernel/arch/x86_64/include/platform.h:8` 把 `PHYS_MEMORY_END` 写死为 `0x40000000`（1 GiB），
`arch_ram_range()`（同文件 :17）只返回 `[0, 1 GiB)`；所以即便 QEMU 给 `-m 2G`，内核仍只管理 262144 个页框
（启动日志 `[PFA] total_frames=262144`、`[MM] ... 256061 free (1000 MB)`）。

高位直接映射已经覆盖物理 0–2 GiB（`kernel/arch/x86_64/boot/entry.S:74-77` 用两个 1GB 大页映为 RAM/WB），
所以扩大上限只缺"知道真实 RAM 大小"。正确做法：在 `_start`（32 位段、`cld` 之后、清 BSS 之前）把 multiboot
的 magic(EAX) 与 info 指针(EBX) 存进 `.data` 全局，再解析 `multiboot_info.mem_upper`；或走 fw_cfg 的
`etc/e820`。1–2 GiB 区在 q35 上可能含 ACPI/固件保留区，不能直接整段当可用帧，否则分配器会把保留页发出去。

修复（`884ef373`）：`_start` 现在在清 BSS 前把 multiboot magic(EAX)/info(EBX) 存进 `.data`，
`firmware.c` 解析 multiboot 内存图：取 type 1（available）区间、丢掉低于 1 MiB 的部分（IVT/BDA/legacy hole）、
按页对齐、并裁到 `entry.S` 直接用 1GB 页映射的 2 GiB 窗口；取不到就回退旧的 1 GiB，不回归。
实测客体：`[RAM] usable 0x100000..0x7ffd9000 (2046 MiB)`、`[PFA] total_frames=523993`（原 262144）、
`Buddy+Slab ... 2020 MB free`，桌面与 ffplay 照常。

关联一条待验证的线索（非结论）：JVM 不带 `-Xms/-Xmx` 时报 `Too small maximum heap`（`/usr/bin/java`
封装已用 `-Xms256m -Xmx512m` 绕过），疑似与其读到的内存 ergonomics 有关；1 GiB 对 Minecraft 也偏紧。

### 视频播放：已默认用 ffplay（mpv 在 A20OS 上不可靠）

`ffplay` 已是视频 MIME 的默认处理器（`7ff4b14c`）：overlay 里放 `ffplay.desktop`，
`/etc/xdg/mimeapps.list` 指向它，客体 `xdg-mime query default video/mp4` 返回 `ffplay.desktop`，
双击视频即用它播放。实测客体播 5s H.264 全片跑完（窗口路径、`-autoexit` 干净退出），
`ffmpeg` 解码同一文件也是 0 错。

mpv 不可靠，是因为它把每个内建 Lua 脚本（osc/stats/console/…）跑在各自线程里，会踩到上面那条
per-thread 状态 bug → SIGSEGV。`load-scripts=no` 关不掉这些内建脚本，崩溃只是从 `lua/stats`
换成 `lua/console`；旧 conf 里的 `stats=no` 更是无效选项（mpv 0.40 没这个 option），会让 mpv 每次
启动报错。那行非法配置已删，`load-scripts/osc/ytdl=no` 保留作缓解，mpv 仍不可靠。**用 ffplay。**

两个坑：`ffplay -nodisp`（无显示）会 `Failed to open file ... or configure filtergraph`，窗口路径正常；
被 SIGTERM 杀掉时 ffplay 会 SIGSEGV 退出，播完或 `-autoexit` 则不会。

用法：双击视频文件，或 `Super+Enter` 开终端 → `ffplay -autoexit /usr/share/a20-media/<视频>`。

### JVM：需要 `/usr/bin/java` 封装（exec_path 不解析符号链接 + 堆参数）

现象是 `java -version` 报 `Error loading shared library libjli.so: No such file or directory (needed by java)`，
而直接用真实路径 `/usr/lib/jvm/java-21-openjdk/bin/java -Xms256m -Xmx512m -version` 正常输出
`openjdk version "21.0.12"`。

根因链已核实：`libjli.so` 在 `/usr/lib/jvm/java-21-openjdk/lib/`，`java` 二进制靠 RPATH
`$ORIGIN:$ORIGIN/../lib` 找它；musl 的 ldso 用 `readlink("/proc/self/exe")` 展开 `$ORIGIN`
（`user/external/musl/ldso/dynlink.c` 的 `fixup_rpath()`）；而 A20OS 的 `/proc/<pid>/exe` 原本是普通文件
（`readlink` 返回 `-EINVAL`），`exec_path` 也只做「绝对化 + 归一化」、不解析符号链接
（`kernel/proc/exec.c:855` 起），于是 `$ORIGIN` 停在 `/usr/bin`。

`/proc/<pid>/exe` 与 `/proc/<pid>/cwd` 现在是 magic symlink（`kernel/fs/procfs/procfs.c`：vnode 类型 +
`procfs_readlink`），实测 `ls -l /proc/self/exe` 已显示 `-> /bin/ls`，这一半已修。

另一半仍未修：`exec_path` 不解析符号链接，经 `/usr/bin/java`（符号链接）启动时 `$ORIGIN` 与 libjli
推导的 `java.home` 都落在 `/usr/bin`。正解是在 `kernel/proc/exec.c` 记录 `exec_path` 时做 realpath
（`vfs_resolve`）；当前用 overlay 的 `/usr/bin/java` 薄封装绕过，直接 exec 真实路径。

堆参数是另一件事：不显式给 `-Xms/-Xmx` 时 VM 报 `Too small maximum/initial heap`（guest 内
`MemTotal` 1048576 kB、`MemAvailable` 541068 kB）。封装默认 `-Xms256m -Xmx512m`，命令行参数仍可覆盖。
`MemTotal` 只有 1 GB（该实例 QEMU 给的是 2G）也值得单独核对 `pfa.total_frames` 的口径。

### 桌面壁纸不显示（backdrop 不渲染）

xfdesktop 在跑（桌面图标正常、root 被背景层覆盖），桌面却是纯黑：1024x768 下 94% 像素为 `(0,0,0)`。

第一轮把 key 匹配排除掉了。`/backdrop/screen0/monitor<id>/workspace0/last-image` 的 `<id>` 用的是
`sha1(connector)`，而 `sha1("Virtual-1")` 正好在 overlay 的 9 个 monitor key 里；`image-style=5`、
`color-style=0` 合法；改成 SVG 壁纸并显式加 `image-show=true` 后仍然全黑。顺带发现镜像里没有
jpeg/png 的 gdk-pixbuf loader（`loaders.cache` 有 ani/bmp/gif/icns/ico/pnm/qtif/svg/tga/tiff/xbm/xpm），
`gdk-pixbuf` 也不依赖 libjpeg/libpng，所以 `xfce-blue.jpg` 本来就无法解码，这也是已改指向
`xfce-flower.svg` 的原因。

第二轮转到 guest 内实测。`xfconf-query -c xfce4-desktop -lv` 里 backdrop 树完全正确：
`monitor0/workspace0` 与 9 个 `monitor<sha1(connector)>/workspace0` 全是 `image-show=true` /
`image-style=5` / `last-image=/usr/share/backgrounds/xfce/xfce-flower.svg` / `color-style=0`；SVG 文件
确实存在（16972 B），`libpixbufloader_svg.so` 在 loaders.cache 里注册了，`librsvg-2.so.2`(2.61.2)
也在镜像里。解码链（gdk-pixbuf → svg loader → librsvg）看起来应该可用，`last-image` 指向的文件也在，
画面却仍全黑，于是第二个原因只能在渲染侧：xfdesktop 的 backdrop 窗口（layer-shell 表面）到
labwc(WLR_RENDERER=pixman) 合成这一段，或者 xfdesktop 对 SVG 的 rasterize 失败。

第三轮二分 decode 与 render：把壁纸换成一个 `loaders.cache` 一定支持的 BMP（宿主造了张纯红
64×64 BMP，`debugfs` 注入 `/root/red.bmp`，两个 monitor key 都改成它并 restart xfdesktop）。
截图里红色像素为 0，画面还是纯黑。连 BMP 都不渲染，问题就不在解码，而在「backdrop 上图 / 合成」
这一段。同一轮还出现了 `SIGSEGV: pid=163 code=13 stval=0x12`（用户态空指针附近访问），很可能是
xfdesktop/缩略图相关组件在画 backdrop 时挂掉。下一步看渲染侧：`xfdesktop` 加
`--enable-debug`/`G_MESSAGES_DEBUG=all` 看 backdrop 的加载与绘制，确认 backdrop 的 layer-shell 表面
是否真的 `commit`（labwc 日志 / `wayland-info`），并查那个 `stval=0x12` 的 SIGSEGV 属于哪个进程。

第四轮做 xfdesktop 全量 debug。`G_MESSAGES_DEBUG=all xfdesktop` 输出里没有任何
backdrop/image/draw 相关日志，只有 dconf/GTK/GIO 的常规初始化
（`Compositor prefers decoration mode 'server'`、`Connecting to session manager`、
没有 session manager/portal、`mnt_monitor_get_fd failed: Operation not permitted`）。
也就是说 xfdesktop 根本没走到「加载并绘制 backdrop」（或那一段无日志），
backdrop 表面上没有被真正画出来。

第五轮查 xfdesktop 版本与协议。镜像是 xfdesktop 4.20.1，链接了 `libgtk-layer-shell.so.0`；二进制里有
`Your compositor must support the zwlr_layer_shell_v1 protocol` 这条串，说明它的 Wayland backdrop 依赖
wlr-layer-shell。但日志里并没有这条报错，也没有任何 backdrop 行；同时 `xfdesktop --version` 正常、
xfce4-panel 正常。收敛判断是 xfdesktop 4.20.1 的 Wayland backdrop 很可能根本没被绘制（该版本 Wayland
支持较新且有缺口），而不是协议缺失或解码失败。可行修法：改用合成器层面的背景工具（`swaybg`/`wbg`
之类，仅需一张图并挂 background 层），或在 xfdesktop 里确认 Wayland backdrop 的开关/补丁。镜像里目前
没有 swaybg/wbg/hsetroot/feh 任何一个。

第六轮按上一轮的修法把 `swaybg`(1.2.1) 加进 `xfce` world 并在 `start-desktop-components.sh` 里启动，
重制镜像后验证。swaybg 确实起来了并配置成功
（`[main.c:282] Found config * for output Virtual-1 (AOS A20OS Display 0x00000001)`，
进程存活 12s+ 未退出），但截图里依然没有背景：

  - `swaybg -i /usr/share/backgrounds/xfce/xfce-flower.svg -m fill` → 桌面区仍是纯黑；
  - `swaybg -m solid_color -c 00aa22`（纯绿）→ 绿色像素 0；
  - `pkill -x xfdesktop` 之后再截图 → 仍是纯黑（排除「xfdesktop 用黑表面盖住背景层」）。

两种模式（图片 / 纯色）都失败，说明不是解码、也不是模式选择，而是 labwc 没有把 background 层的
wlr-layer-shell 表面合成进 scanout；同一会话里 panel 所在的层却正常渲染。收敛到这里，壁纸真正的
阻塞点是 labwc / wlr-layer-shell 的 background 层合成路径，不是「缺一个背景工具」，也不是 xfdesktop
的 backdrop 开关（第四、五轮的结论需要按此修正）。下一步：在 labwc 侧看 background 层表面的
map/commit 与 pixman renderer 的 damage 处理，单独排除 `WLR_RENDERER=pixman`、
`WLR_NO_HARDWARE_CURSORS=1`、`WLR_DRM_NO_ATOMIC=1` 三个开关。

第七轮换时间维度采样，结论是第六轮错了。第六轮的截图都是在启动 ~60s 之后拍的，而壁纸在那之前就
已经画出来过又被盖掉。用户观察「刚启动有壁纸，过一会儿变黑」后按时间重采（headless，每 5s 一张，
统计桌面区非黑像素）：

  | t(s) | 颜色数 | 最大占比色 | 黑像素 |
  |---|---|---|---|
  | 5–20 | 1–2 | (0,0,0) | ~100% |
  | **25** | **202** | **(0,144,188)** | **0.0%** |
  | 30+ | 26 | (0,0,0) | 99.4% |

swaybg 的壁纸确实画上去了（t=25s），5 秒后就被纯黑盖掉。
决定性实验：把 `spawn xfdesktop` 换成一句 echo（不启动 xfdesktop）后重采，壁纸从 t=25s 一直保持到
t=110s（202 色 / 0% 黑）。凶手就是 xfdesktop：它不画配置的 backdrop，而是铺了一块**不透明的黑色
桌面表面**，把 background 层上的壁纸整个盖住。第六轮「`pkill -x xfdesktop` 之后仍是纯黑」之所以
误导，是因为那时黑面已经合成进 scanout，杀掉进程并不会让合成器回到上一帧。

修法已验证：xfdesktop 留着（桌面图标照常），但让 swaybg 在它之后再创建自己的 background 表面，
wlr-layer-shell 同一层内后创建的表面在上面。实测「等 xfdesktop 进程出现 + settle 20s 再起
swaybg」：壁纸从 t=54s 稳定保持到 t=144s（229 色 / 0% 黑），面板与图标同时在。代价是启动后约 30s
内桌面是黑的；若日后能把 xfdesktop 的 Wayland backdrop 修好、或让它别铺那块黑面，这段等待就可以去掉。

顺带：同一批次里 `tumblerd` 以 `code=1`(#GP, `insn 0f b6 48 ..`) 崩溃多次（`FATAL: pid=... comm=tumblerd`），
是独立的用户态坏指针崩溃。同一轮还抓到一个确定的用户态崩溃：`FATAL: pid=140 ... comm=tumblerd`，
内核侧是 `ADE/ALE: pid=140 sepc=0x40226031 stval=0x8136a00e code=1`，又是 #GP（`code=1`，
x86_64 上只由 #GP 产生；`stval` 对 #GP 是过期 CR2），`insn@sepc=0x48b60ff4`（`0f b6 48 ..` 一个字节 load）。
即缩略图守护进程 tumblerd 在解引用一个坏地址而挂（xfdesktop 用它给桌面图标出缩略图）。这与 JVM
最初那条 #GP 同类（非规范地址/坏指针），是另一个独立的用户态崩溃，值得单独查。

### guest 写盘后 ext4 位图校验和不一致

`image-world` 产物 `e2fsck -fn` 干净；但 guest 启动一次后，从宿主机看（QEMU `-snapshot` 的 qcow2
overlay，或 kill 后的镜像）会出现 `Block bitmap checksum does not match bitmap`，`debugfs` 直接打不开。

这解释了「测试环境注意事项」里那条「损坏来源未定位」：**损坏不是构建期引入的，是 guest 写入时产生的**。
损坏的 rootfs 会伪造内核 bug（udev worker 超时、应用起不来），排查前务必先 `e2fsck -fn`。

怀疑 A20OS 的 ext4 写入路径没有同步 `metadata_csum` 的位图校验和（`^has_journal` 下没有日志回放来兜底）。
需要在内核 ext4 写路径上核对 group descriptor/bitmap 的 checksum 更新。

### 面板下拉菜单项不启动应用

面板按钮能开菜单，但点下拉菜单项既不启动应用（无新 `xdg_surface`、无窗口），也不关菜单。

在 `e2fsck` 干净的 rootfs 上复验，指针移动与坐标都正常：悬停分类项会弹出子菜单，点击面板按钮会打印
`press on layer-(sub)surface`。这与上面已解决的「窗口控制键无效」是两个不同症状；窗口控制/聚焦/拖动
已由 labwc 默认绑定修复，本条的客户端菜单条目点击仍未定位。

已排除内核输入通路本身。`struct input_event` ABI、PS/2 按键解码、每包 `SYN_REPORT`、evdev 打戳、
内核与 vDSO 的 CLOCK_MONOTONIC 时基逐项核对一致；libinput 也枚举到了设备（`Adding A20OS evdev mux [0:0]`）。

是否打日志取决于命中节点的类型（只有层表面分支会打印），所以「落在子菜单上的按下不打印任何 labwc
日志」本身是正常的，不能当作事件没送达的证据。下一步应在 guest 内直接看 `wlr_scene_node_at` 的命中结果
与客户端实际收到的 `wl_pointer.button`，不要再从截图推断。labwc 会把每个执行到的 action 记进日志
（`[action.c] Handling action <n>: <Name>`），这是比截图可靠的观测手段。

### dbus 同步调用超时 / 对端 pid 为 -1

会话里 `NoReply`，xfsettingsd/panel 报 Xfconf/dconf 超时后降级；dbus 日志里身份异常：
`dbus-daemon[44]: [session uid=0 pid=18446744073709551615 pidfd=5] ...`（pid 为 -1）。在 `e2fsck`
干净的 rootfs 上稳定复现。同一行里 `requested by ':1.7' (uid=0 pid=92 comm=".../xfsettingsd")` 的 pid
却是对的。

SO_PEERCRED 那一半已修：`kernel/net/socket_unix.c` 的 connect 路径把发起端自己的 pid 写进了
`s->peer_pid`，而发起端的对端应是服务端。现在 bind 时记录属主凭据（`net_socket_t.owner_pid/uid/gid`），
connect 时用它给发起端填 `SO_PEERCRED`；accept 出的子 socket 那一路原本就对。

仍未修的是 pid 本身：改完后 dbus 日志里的 `pid=18446744073709551615` 依旧。同一行 `pidfd=5` 说明
`SO_PEERPIDFD` 已经拿到 pidfd，所以 dbus 显示的这个 `pid=` 不是取自 `SO_PEERCRED`，而是 `SO_PASSCRED`
的 `SCM_CREDENTIALS`（`ch_cred_*` 路径）。下一步查 AF_UNIX 首包（dbus 的初始 NUL 字节）的
SCM_CREDENTIALS 为什么是 -1。已排除 `peer_pid` 未初始化（`net_socket_alloc()` 用
`obj_cache_alloc_zero()`）与 `getpid()` 本身（`dbus-daemon[44]` 说明任务 pid 正确）。

### 其他架构的 FPU/上下文

riscv32、ppc64le 已补上 trap 帧的 FP 保存（ppc64le 仅编译验证）；arm32 仍未修，本机无 arm 工具链，
且其 trap 帧与 trap.S 用逐字段断言强绑定，改动未经验证。

判定标准是「用户态是否启用 FP/SIMD」：arm32（`-mfpu=vfpv3-d16 -mfloat-abi=hard`）、riscv32（`ilp32d`）、
ppc64le（`MSR_FP`）都启用；loongarch32/armv7m 无 FPU，不受影响。ppc64le 的向量保存受 `MSR_VEC`
门控而该位未设，需另存 FPR。

### 32 位架构构建

riscv32 在 HEAD 上有多处既有编译错误（`arch_vdso_counter` 声明、`pfa_range_t` 断言、`proc.c`
type-limits 等）；前两处已修，`proc.c` 等仍待处理。非 VDSO 架构（riscv32/arm32/loongarch32）都会撞到
`arch_vdso_counter` 声明缺失。

### x86_64 桌面：lwIP IPv6 收包路径 pbuf 引用计数被破坏

> **当前状态（先读这一段；下面是调查过程，含已被推翻的中间结论）**
>
> 1. 找到并修掉了一个真的双重释放，位置在 raw socket 接收回调
>    （`kernel/net/socket_inet.c` 的 `raw_recv`）：它先 `pbuf_free(p)`，然后返回 0。
>    lwIP 把这个返回值读作"这个包已被吃掉"的标志，非零才表示回调接管了所有权；
>    返回 0 等于宣称自己没动，于是调用方又释放了一次。`raw_recv` 同时注册在 AF_INET 与
>    AF_INET6 的 `SOCK_RAW` 上。这是 send 侧那个已修双重释放的接收侧镜像。
> 2. 但它是否是本 panic 的触发原因，未证明。修掉它不等于解释了这个 panic。
> 3. IPv6 专属这个相关性仍未解释。唯一能对得上的方向是：同一条路径上
>    引导期 IPv6 流量（RS/NS/NA/DAD、MLD）的量远高于 IPv4，足以把一个
>    潜伏的竞争暴露出来，而 IPv4 负载把它盖住了。**这是解释，不是证明。**
> 4. "真正的重复释放 vs 相邻缓冲区越界写"这对假设，已经用测量判开了，
>    结论与此前记录的方向相反：见下方"已定案"小节。**成因是 pool 越界写，不是重复释放。**
> 5. ~~下一步：用 `CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1` 构建以抓第一个失衡。~~
>    已做，并且它一次性给出了判据（下方）。该选项仍默认关闭。
>
> **已定案（`CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1`，x86_64 + KVM 桌面回归，
> `a96134c2` 之后）**
>
> 开 canary 后同一场景不再报 `pbuf_free: p->ref > 0`，而是报：
>
> ```
> ========== KERNEL PANIC ==========
> lwIP assertion failed: detected mem underflow in pool PBUF_POOL
> [PANIC] backtrace:
>   [0] mem_overflow_check_raw
>   [1] do_memp_malloc_pool_fn
>   [2] pbuf_alloc
>   [3] a20_lwip_process_netif_rx_tx_locked
>   [4] ethernet_output ... [IRQ] ... idle_loop
> ```
>
> 三件事因此确定：
>
> 1. **是 pool 越界写，不是重复释放。** 重复释放只会触发 `p->ref > 0`；
>    canary 完好时它就该那样报。它报了 underflow，说明在断言触发之前，
>    某个 pool 元素前面的受限区已经被改写。
> 2. **是 underflow（越界写到元素之前），不是 overflow（之后）。** pool 是
>    `struct pbuf` 的连续升址数组，所以"写到 N 号元素之前"落进的是
>    N-1 号元素的尾部。即：破坏源缓冲区写过了它的低端，其越界末端
>    正好落在前一个 pbuf 上。
> 3. **发现在分配路径上。** 崩溃点是 `pbuf_alloc` 取新元素时检查到上一个
>    元素已被污染，说明破坏发生在这次分配之前，与 IPv6 解析无关。
>    IPv6 只是流量形态；它把这条路径的包量和包长分布推到了会踩中的区间。
>
> **首要嫌疑（机制已查清，尚未证实触发）**：`ip6_frag.c` 的 IPv6 重组助手。

`IPV6_FRAG_COPYHEADER 1` 是 A20OS 在 `lwipopts.h` 里唯一一处偏离 lwIP 默认值的 IPv6 配置，
其自带注释就写明「64-bit targets cannot fit lwIP's IPv6 reassembly
helper into IP6_FRAG_HLEN」。开启后：

- `IPV6_FRAG_REQROOM = sizeof(struct ip6_reass_helper) - IP6_FRAG_HLEN`
  = **12 − 8 = 4**（64 位下 `struct pbuf *` 占 8 字节）；
- `ip6_frag.c:415` 用 `pbuf_header_force(p, 4)` 把 payload 指针往回挪 4 字节，
  就地覆盖片外扩展头；
- 而 `pbuf_add_header_impl()` 的越界检查只对连续型 pbuf 存在
  （`payload < p + SIZEOF_STRUCT_PBUF`）。非连续型 pbuf 走 `force` 分支时
  完全不做检查，直接 `payload - 4`；
- 注释声称「This cannot fail since we already checked when receiving this fragment」，
  但那个「already checked」是 `ip6_frag.c:289` 的
  `p->len >= sizeof(struct ip6_frag_hdr)`：它只验证片头往后放得下，
  完全没有验证前面有没有 4 字节可借。

若该片落在非连续 pbuf（例如 PBUF_POOL 链上的元素）上，这 4 字节就直接写进
前一个元素的 payload 尾部 / `struct memp` 空闲链表指针，与 canary 报出的
「underflow in pool PBUF_POOL」完全吻合（underflow 与前一个元素的 overflow 是
同一处物理写坏，只是被哪一侧的检查先发现）。

这条假设能同时解释全部四个观察：仅 IPv6（`ip6_frag` 是 IPv6 专属路径）、
写坏 pool、underflow 而非 overflow（往 payload 之前写）、
在分配路径被发现。

**已证伪（保留记录）**：上面的 `ip6_frag` 假设经实测不成立。以
`LWIP_IPV6_FRAG=0`（其余配置不变、canary 保持开启）重建并运行 xfce 桌面，
panic 原样复现：

```
lwIP assertion failed: detected mem underflow in pool PBUF_POOL
[3] a20_lwip_process_netif_rx_tx_locked
[2] pbuf_alloc
[1] do_memp_malloc_pool_fn
[0] mem_overflow_check_raw
```

与开启分片时的栈完全一致，因此写坏内存的不是 IPv6 分片重组。
附带一条观测：该次 panic 出现在启动期（elogind 刚起来，桌面未起），
说明损坏在首次网络活动时就已经存在，而不是桌面负载才触发。

**canary 语义的精确含义**（`memp.c:130` 传入 `payload = element + MEMP_SIZE`，
即 `struct memp` 之后的 8 字节对齐偏移）：

- BEFORE 保护区 = `[payload - 16, payload)`，其中 8 字节落在前一个元素的
  payload 尾部，另外 8 字节是本元素的 `struct memp` 空闲链表指针；
- 保护区在 free 时填 `0xcd`、alloc 时校验，所以 alloc 期报错意味着
  「该元素被 free 之后，仍有代码往它的 payload 起始处回写」。

**二分结果（canary 全程开启，均为真负）**：

| 配置 | 结果 |
| --- | --- |
| `LWIP_IPV6=0` | **无 panic**，桌面正常启动并跑满 400s 超时 |
| `LWIP_IPV6_FRAG=0` | panic 复现，栈不变 |
| `LWIP_ND6=0` | panic 复现 |
| `LWIP_IPV6_DHCP6=0` | panic 复现 |
| QEMU 不挂 `-device virtio-net-pci` | **无 panic**，桌面跑满 300s 超时 |

所以损坏必须有 IPv6 才发生（IPv6 是必要条件），但与分片重组无关，
也与邻居发现无关。`LWIP_IPV6=0` 这一档同时给出了修复方向的判据：任何
最终修法都应当能在保留 IPv6 的前提下成立，而不是关掉 IPv6 绕过。

**配置二分已彻底用完**：剩下的候选无法再用编译期开关排除。

| 尝试 | 结果 |
| --- | --- |
| `LWIP_ICMP6=0` | 编译失败：`ip6.c:780` 无条件调用 `icmp6_param_problem()` |
| `LWIP_RAW=0` | 编译失败：`lwip_stack.c:470` 使用 `MEMP_RAW_PCB` |

**当前最强嫌疑：`ip6_input:1054` 忽略了 `pbuf_add_header_force()` 的返回值。**

```c
#if LWIP_RAW
  pbuf_add_header_force(p, hlen_tot);   /* 返回值被丢弃 */
  raw_status = raw_input(p, inp);
```

它同时满足此前所有观察：位于 `ip6_input`（仅 IPv6）；对每个 IPv6 包都会
执行（因此「首次网络活动即损坏」，不需要桌面负载）；且在 RX 得到的
`PBUF_POOL` pbuf 上，这次 force 必然静默失败。pool 的 payload 紧贴
`struct pbuf` 尾部，回退 1 字节就越过
`payload < p + SIZEOF_STRUCT_PBUF` 这道检查，于是 force 什么都不做，
而代码却当作成功继续执行；此时 `ip_data.current_ip_header_tot_len` 已被写成
`hlen_tot`，`raw_input()` 拿到的是一个 payload 布局与 `hlen_tot` 不符的 pbuf。

A20OS 侧确实存在 raw socket（`kernel/net/socket_packet.c`），且同一区域此前
修过一次双重释放，与「pool 元素被写坏」的症状方向一致。

**进一步推论（重要）**：`raw_input()` 在没有任何匹配的 raw pcb 时会提前返回，
什么都不做。也就是说，仅靠 `ip6_input` 这一行不足以致害，必须同时存在一个
绑定了该协议的 raw socket。这正好把嫌疑引向 A20OS 自己的代码：
`net_packet_rx_defer()` 会在每个收到的帧上喂一次 raw socket 通道，
与「首次网络活动即损坏」的现象吻合。若该推断成立，真正的越界写发生在
`kernel/net/socket_packet.c` 消费 raw pbuf 时按（被 force 失败弄脏的）
IP 头去算长度的那一段，而不在 lwIP 内。

因此下一步 instrument 应当同时覆盖两处：`ip6_input:1054` 的 force 失败分支，
以及 `socket_packet.c` 里 raw 接收的长度计算。只看 lwIP 侧可能看不到越界写。

**已证伪**：`ip6_input:1054` 这条线索经实测不成立。按其机制改写
（仅在 force 成功时才执行 undo）并以 canary 重建后，panic 原样复现，
栈帧 0~3 完全一致（仅 `[3]` 偏移因代码布局变化从 `+0x15c` 变为 `+0x16b`）：

```
lwIP assertion failed: detected mem underflow in pool PBUF_POOL
  [0] mem_overflow_check_raw   [1] do_memp_malloc_pool_fn
  [2] pbuf_alloc               [3] a20_lwip_process_netif_rx_tx_locked+0x16b
```

该改动已回退，vendored lwIP 保持未打补丁状态。

顺带更正上一条提交里的一个错误推论：`raw_input()` 在没有匹配 pcb 时确实会
提前返回，但它返回的是「未吃掉」，因此 `pbuf_remove_header` 仍会执行。
也就是说这个缺陷不需要存在活动的 raw socket 就会触发，先前「必须同时有
raw socket」的推断是错的。

**一条尚未解释的观测**：栈帧 `[4]` 是 `ethernet_output+0x13f2ae827`。
偏移量约 5.4×10⁹，不可能是任何函数的合法偏移，说明符号化落到了最近的
前驱符号上，即该帧的返回地址无法解析。若这是真实的栈损坏而非 unwinder
的缺陷，则越界写的目标可能不止 pool，还波及到了内核栈。这与「1~8 字节溢出」
的判断并不矛盾，但目前无法区分。

**仍未定案**：到底是哪一个缓冲区越界写了。下一步应在该处加
instrumentation，检查 force 的返回值，在失败分支打印 `p->payload` 实际地址
与 `hlen_tot`，直接确认 `raw_input` 是否被喂了错位的 pbuf。

### 本轮（`feat/graphics-3d-completion`）新证据：篡改规模被实测推翻，且拿到一个可用的规避

**1. 重组假设两半都已排除。** 上文只验证过 `LWIP_IPV6_FRAG=0`（**发送**方向
分片）。本轮补测 `LWIP_IPV6_REASS=0`（**接收**方向重组），panic 依旧；`ip6_frag.c`
的重组助手这条线不再是候选，写坏内存的是别的 IPv6 路径。

**2. 篡改规模比上文推断的大得多，而且写进去的是指针。** 临时给 `mem.c` 的
`mem_overflow_check_raw()` 加了转储（已回退），实测越界写入的内容是：

```
guard[16]  = e07d40000080ffff 48b42f010080ffff
           = 0xffff800000407de0  0xffff8000012fb448   ← 两个内核虚拟地址
pbuf[0..15] = 0600000000000000 98d281000080ffff
```

两处修正上文：

- 写坏的**不止 1~8 字节**：整个 16 字节 guard 区**全被覆盖**，首个被查出的偏移是
  `-16`，即紧邻元素那一端的最后一个字节。
- 覆盖进去的是**两个相邻的内核虚拟地址**，不是零散包数据。这排除了「拷贝多了
  几个字节」这一类解释，指向**某个含指针字段的结构体被写在偏移错误的位置**，
  或某个 pbuf 的 `payload` 指针算错后经它写入。

**3. 排除项。** `memcpy`（`core/string.c`）只会前进且严格写 `n` 字节，无法写到
目标之前；`pbuf_add_header_impl()` 对 `PBUF_POOL`（该类型带
`PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS`，payload 紧贴 `struct pbuf`）的越界检查是
**有效**的，不是先前推测的「空检查」。

**4. 可用的规避（已实测；是规避，不是修复）。** `LWIP_IPV6=0` 时，带
`virtio-net-pci` 的 xfce-x86_64 桌面**连续运行 9 分钟无 panic**，IPv4 正常
（`netif en2 ... ip=10.0.2.15 gw=10.0.2.2`），帧持续 present，桌面可用。代价是
**整个 IPv6 协议栈关闭**（无链路本地地址、无 RA/RS、无 MLD/DHCPv6）。这是产品
取舍，**本轮未擅自提交**，留待决策。

**5. 下一步为什么需要新工具。** 本轮每次桌面启动约 4 分钟，且 panic 处栈回溯不可信
（帧 `[4]`、帧 `[8]`~`[11]` 符号落在 `ethernet_output` 上但偏移是 `0x7f9xxxxx`
量级的垃圾值），靠增加启动次数做二分已不可行。要定位那个写坏内存的指针，需要
**确定性的用户态复现器**（guest 内构造 IPv6 报文直接打 `ip6_input`），或在
`MEMP_PBUF_POOL` 上加硬件 watchpoint——本内核目前**没有** watchpoint 设施
（`x86_64` 只实现了 ptrace 的 regset，`trap.S` 未处理 `#DB`）。

尚未排除的 IPv6 专属面：

- `ip6.c` 输入路径与扩展头处理（`pbuf_remove_header` / `pbuf_unchain` 链式搬移）；
- ICMPv6 中非 echo 的路径（echo 应答已确认走 `PBUF_RAM`，故非 echo 类）；
- IPv6 的 `netif` / 地址层。

完全不挂 virtio-net 也不复现，说明确实必须有一条活的 RX 数据面在喂包，
排除「与网络无关的启动期内存踩踏」。

分片重组、邻居发现、DHCPv6 三条「IPv6 专属且常驻」的后台路径已全部排除，
剩下的多半就在 `ip6_input` 自身的扩展头解析/搬移里。逐个再关子系统
的收益开始下降，建议改为直接在 `ip6_input` 的扩展头循环里对
`pbuf_remove_header` / `pbuf_unchain` 加定位 instrumentation，
用 canary 命中时的 `p->payload` 地址反推真正的越界写点。

结论不变但更精确：存在一处 **1~8 字节的溢出**，写穿某个 1536 字节
`PBUF_POOL` payload 的尾部，落在下一个元素的头部。

**顺带发现的独立缺陷**（非本 panic 的成因，尚未修）：
`lwip_stack.c` 的 RX 循环只用 `len <= 0` 挡住了 `recv()` 的错误返回，
没有上界校验；`len` 随后原样进入 `pbuf_take(p, st->rx_frame, (u16_t)len)`。
若某个 `recv` 实现返回大于 `sizeof(rx_frame)`（1536）的长度，
`pbuf_take` 会越过 1536 字节的 `rx_frame` 读取。当前各 `recv` 实现均未观察到
越界返回，故尚未触发，但这是一处真实的边界缺失，应补上裁剪。

**仍未定案**：到底是哪一个缓冲区越界写了。canary 只给出方向（低端越界、
落在 PBUF_POOL 相邻元素），不给身份。下一步应从 pool 元素尺寸与
`PBUF_POOL_BUFSIZE`（1536）的边界关系入手，找哪个子系统按 1536 以上的
步长写入 pool 附近内存；`MEMP_OVERFLOW_CHECK` 无法回答这个问题，
需要在 canary 命中的那对元素上打印其地址/尺寸/相邻关系。
>
> **仍未被推翻的既有事实**：lwIP 除一处有板级保护的 printf 外未打补丁；
> RX 接缝每帧新分配一个单元素 pool pbuf，且 virtio_net 同时夹紧 `pkt_len` 与
> `used_len`，因此接缝处一帧的 pbuf 不可能被双重释放；出问题的那次释放
> 发生在更早的包上。

- 症状：x86_64 桌面起来后约 1 分钟不确定性地打死内核，报
  `lwIP assertion failed: pbuf_free: p->ref > 0`；调用链为
  `ip6_input` → `ethernet_input` → `a20_lwip_process_netif_rx_tx_locked`。
  桌面会话因此活不过约 55s。这条是既有问题，rebase 到当前 main 后依旧复现。
- 已修（调用侧，真实缺陷）：`kernel/net/lwip_stack.c` 的 RX 循环在 `netif->input()`
  返回错误时又 `pbuf_free(p)`，但 lwIP 的 `ethernet_input` 在自己的错误路径上
  「先释放再返回 `ERR_OK`」（源码注释：so the caller doesn't have to free it again），
  这是对已移交所有权的 pbuf 二次释放。已修（`687f8271`）。
- 已修（可二分性）：`kernel/net/socket_inet.c` 的 UDP/raw 收包回调原先无条件调用
  `ip6_current_dest_addr()` / `IP6H_HOPLIM()` 等，导致 `LWIP_IPV6=0` 根本编不过，
  IPv6 路径无法单独二分。已加 `#if LWIP_IPV6` 保护（`0154a7fc`）。
- 已定位到 IPv6 收包路径：`LWIP_IPV6=0` 时桌面稳定存活 250s+、零 panic。
- **关键坑（务必注意）**：想「只关一个开关」时，我把 `LWIP_IPV6_DHCP6` 与
  `LWIP_IPV6_AUTOCONFIG` 关掉、其余 IPv6 全留，也不复现，但那是因为
  guest 根本没拿到 IPv6 地址，IPv6 收包路径几乎没被触发（日志里仅 1 处 IPv6、
  无任何地址行）。所以这条只是混杂变量的阴性结果，不能据此判定
  DHCPv6/SLAAC 无罪。真正结论是：**panic 需要真实 IPv6 收包流量才会发生。**
- 已排除（逐一验证）：
  - `g_lwip_lock` 确实在 IRQ 路径（`kernel/drivers/net/virtio_net.c:554`）与 poll 路径都持有，RX 循环本身已串行化；
  - `net_packet_rx_defer()` 在自旋锁下 `memcpy` 拷贝帧，不会保留共享 `rx_frame` 指针；
  - vendored lwIP 的重组路径已审：`ip6_reass_free_complete_datagram()` 与
    `ip6_reass()` 完成时的 `pbuf_cat` 链接逻辑均与上游所有权约定一致，未见缺陷。
- 曾判定为"真正的重复释放，不是内存踩坏"，这条判定不成立，见下方"更正"。
  当时的做法是在 `pbuf.c` 里临时记录已释放地址、重新分配时抹掉记录，并在断言前
  比对，从而试图把两种成因分开（`pbuf_free: p->ref > 0` 只能证明 `ref == 0`，
  单看断言无法区分）。x86_64 `NR_CPUS=4` + XFCE 镜像实测输出：
  ```
  pbufdiag: DOUBLE-FREE 0xffff80000147cba8 type=0 len=0 tot=2 next=0x0
  ```
  该地址当时看起来确实被释放过。更正：这个判据在 memp 的空闲链表
  语义下是无效的；空闲链表本身就是穿过已释放块的指针链，一次双重释放之后
  同一块内存会被交给两个活着的 pbuf，于是"这块地址被释放过"对每个 pbuf 都
  成立，`ref` 也失去意义。**所以这一段插桩没有排除"相邻缓冲区越界写"，也没有
  证实"真正的重复释放"。当时的结论应当作废。** 当时被排除的"重复释放"候选
  （`pbuf_remove_header` 内部释放、`ip6_input` 返回非 `ERR_OK`）仍然是有效的
  排除，它们是逐个读实现否掉的，不依赖上述插桩。
- 关键线索是被释放的 pbuf 形态：**`len=0` 而 `tot_len=2`**（`type=0 next=0x0`）。
  `len` 归零的来源已确认：vendored lwIP 的 `pbuf_remove_header()` 不释放任何东西，
  它只把 `payload` 前移并原地减小 `len`/`tot_len`，因此当头部正好等于首个 pool pbuf
  的长度时就会留下 `len=0`、`next=0x0`、而剩余字节挂在 `tot_len` 里的形态。
- **已排除的猜测（不要重查）**：「`pbuf_remove_header` 内部释放了 pbuf，导致上层再
  释放一次」。读实现即可否掉，它只做指针/长度算术。而且该函数只处理链首
  （不遍历 `p->next`），所以链首被吃空后仍有一条 `tot_len=2` 的尾巴，形态自洽。
- **第二个猜测也已排除（不要重查）**：怀疑是「`ip6_input` 释放 `p` 后返回非 `ERR_OK`，
  再被 `ethernet_input()` 的 `if (err != ERR_OK) pbuf_free(p);` 释放一次」。实测
  `ip6_input` 只有 5 个返回点（`ip6.c:535/541/561/581/1119`），全部是 `ERR_OK`，
  根本不存在 `ERR_MEM` 出口；因此 `ethernet_input` 那条错误路径对 IPv6 永不触发。
- 目前两条最自然的 IPv6 内部路径都被排除（见上），重复释放的实际双方还没找到。
  已确认的事实：`ip6_input` 自身不返回错误；链首 pbuf 可被 `pbuf_remove_header`
  吃成 `len=0/next=0x0`；断言触发时 `ref==0`。"该地址确实曾被释放"这一条不再
  可用作证据（见上面的更正）。IPv6 专属这一点仍是最强线索；同一条
  `ethernet_input` 路径的 IPv4 分支从不触发，说明差异在 `ip6_input` 内部而非入口。
- **已做过一次 `ip6_input` 全量打点（22 处 `pbuf_free`）的实验，结果与预期不同**：
  在 x86_64 `NR_CPUS=4` + XFCE 镜像上捕获到
  ```
  ip6diag: REPEAT-FREE 0xffff80000139aa00 already in recent frees
  ip6diag:   ==> 0xffff80000139aa00
  ip6diag:       0xffff80000139aa70
  ```
  但同一轮 `pbuf_free: p->ref > 0` 断言并没有触发（0 次）。也就是说这次捕获到的
  「重复释放」不是本 bug：该 pbuf 当时 `ref` 仍 > 0（很可能是被 `pbuf_ref` 过、
  有两个所有者），两次 `pbuf_free` 本身合法。
  **教训：判重必须同时打印 `p->ref`**，只看地址重复会把正常引用计数误判成缺陷；
  任何后续检测都不要漏掉这一点，否则会得出假结论。
- 另一个观察：两个地址相差 `0x70 = 112` 字节，正好等于 `struct pbuf` 的大小，
  即它们是 pbuf pool 里相邻的两个元素。这与「越界写踩到邻居」的形态吻合，
  但仅凭地址相邻不能下结论（pool 本来就是连续分配），仍需 `p->ref` 与写入点证据。
- 下一步建议（未做）：把上面的 22 处打点保留，但记录项从「地址」扩展为
  `{地址, p->ref, 释放点行号}`，并且只在断言真的会触发的那一轮 dump 序列；
  本次实验说明不崩溃的轮次里重复信息噪声极大。复现需保持 SLAAC 可用。
- **但这套「按地址判重」的路子整体上已经不可用**，原因见顶部第 4 条（memp 的
  空闲链表穿过已释放块，一次双重释放之后地址判重与 `ref` 都会失真）。
  正确方向是 `CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1` 抓第一个失衡点，而不是继续
  在触发断言的那一轮里做模式匹配。

### x86_64 桌面：间歇性 `Failed to set CRTC`（ENOENT），显示起不来

wlroots legacy 后端反复报
`[backend/drm/legacy.c:122] connector Virtual-1: Failed to set CRTC: No such file or directory`，
桌面没有画面，而且是间歇性的：同一份配置有的运行一次都不报，有的报 20 次。

内核侧唯一来源是 `drm_mode_setcrtc()` 中 `drm_find_gem(ctx, c.fb_id)` 返回 NULL
→ `-ENOENT`（`drm.c` 内该函数仅此一处 `-ENOENT`）。逐一验证排除了四种可能：

- GEM 表耗尽：插桩统计整个运行期只有 2 次 `drm_gem_alloc`（`slot=0`），
  `DRM_MAX_GEMS 64` 远未用满，表满的打印一次都没出现；
- `MODE_DESTROY_DUMB` 释放了仍被帧缓冲引用的 GEM：插桩后该 ioctl 调用次数为 0；
- `GEM_CLOSE` 释放了仍被引用的 GEM（即本分支 GEM_CLOSE 改动的嫌疑）：插桩后同样为 0，
  该 ioctl 在这条链路上根本不被调用；把 GEM_CLOSE 改成 no-op 也不改变现象；
- 内核与用户态 `drm_mode_crtc` ABI 不一致：本以为内核 `struct drm_mode_crtc`
  （`set_connectors_ptr`/`count_connectors`/`mode_valid`/`mode`）与 Linux UAPI
  不同构会导致 `fb_id` 读偏。原先这里给的依据是错的：它引用
  `user/external/mlibc/sysdeps/managarm/generic/drm.cpp` 说是"走同一份 vendored
  定义"，但那个文件不在 A20OS 构建路径里。A20OS 用
  `tools/targets-mlibc.mk` 配置 mlibc 的 `sysdeps/a20`，而 `sysdeps/a20`
  不含任何 DRM 代码。真正的用户态是 Alpine 的 libdrm（stock apk，跑在
  Linux syscall ABI 上），其 `drm_mode_crtc` 就是 Linux UAPI 的那一份。
  结论侥幸成立，但依据已更正；现在这条 ABI 一致性由
  `tools/check-drm-abi.sh` 门禁按结构体布局逐字段核对，见
  [graphics/3d-graphics.md §8.1](graphics/3d-graphics.md)。

影响面：这条同时卡住 XWayland 呈现验证与 x86_64 `smp>1` 挂起调查，两者都需要
一块真正能出画面的显示器。

根因已定位（插桩实证）。把 `drm_gem_alloc` / `drm_free_gem` / `addfb` / `setcrtc`
四处放在一起打点，并关掉 IPv6 以排除 lwIP panic 干扰（这样桌面才活得久到能出
trace），x86_64 `NR_CPUS=4` 跑出来的序列是决定性的：

  ```
  [GP] alloc h1
  [GP] free h1
  [GP] setcrtc fb_id=1 MISS live=0
  [GP] setcrtc fb_id=2 MISS live=0
  ```

即帧缓冲的后备 GEM 在帧缓冲仍在使用时就已被释放。原因在实现里：
`drm_mode_addfb/addfb2` 直接把 `fb.fb_id = b->handle`，帧缓冲 ID 就是 GEM
handle 的别名，内核里没有独立的 framebuffer 对象，也没有任何引用计数。于是
wlroots 建完 `drmModeAddFB2` 后按 Linux 语义销毁 dumb buffer（`MODE_DESTROY_DUMB`
或 `GEM_CLOSE`），`drm_free_gem()` 就把那个 GEM 释放了；帧缓冲的 `fb_id` 随之悬空，
随后 `drmModeSetCrtc` 带着这个 `fb_id` 回来，`drm_find_gem()` 自然找不到 → `-ENOENT`。
这同时解释了「间歇性」：取决于 destroy 与 setcrtc 的先后。

正确修法已做：引入真正的 framebuffer 对象（`drm_fb_t`，持有对 GEM 的
引用），`ADDFB/ADDFB2` 从独立 id 空间分配 `fb_id`（`g_fbs[]`，
`DRM_MAX_FBS = 64`，`g_fb_next_id` 递增），`RMFB` 释放该引用。同批落地：

- CRTC 现在真的保存绑定（`g_crtc`，此前 `SETCRTC` 呈现成功并返回 0 却什么都不
  存，于是 `GETCRTC` 永远报 `fb_id 0`）；
- `PAGE_FLIP` 不再把 `pf.fb_id` 当 GEM handle 解析（framebuffer id 与 GEM handle
  来自两个独立计数器，此前只是碰巧相等才工作）；
- `GETFB2` 从后备 GEM 应答（此前是一个清零就返回成功的 stub）。

`mode_valid` 在 `fb_id == 0` 时故意保持 1；它表示"CRTC/connector 这一对
已编程了一个 mode"，不是"已绑定 framebuffer"，报 0 会让 wlroots 在 backend
init 阶段直接放弃这个 output。不能只在 `destroy_dumb` 上打补丁绕过，那只是把
悬空推迟到下一次。

复现要点：必须关掉 IPv6（`LWIP_IPV6=0` + `LWIP_ICMP6=0` 等）桌面才活得够久、
不被 lwIP panic 打断；且 trace 要在同一轮 boot 里同时打四处，跨轮次对比会自相矛盾。

## 三、测试环境注意事项

- 测试前先 `e2fsck -fn` 校验镜像，损坏的 rootfs 会伪造出内核 bug。实测一个报
  `Block bitmap checksum does not match`、`Entry 'sys' ... unused inodes area` 的镜像，
  会让 6 个 udev worker 全部 `timeout; kill it`；换干净镜像后为 0。损坏镜像还会造成
  `failed to create cairo scaled font`、应用起不来等假象。新 `image-world` 产物
  `e2fsck -fn` 是干净的（5 个 pass 全过），说明损坏不是构建期引入、而是之后发生，
  具体来源未定位。
- rootfs 构建：直接 `apk add` 走官方 CDN 会慢到像卡死；用预置的 `a20rootfs-builder`
  docker 镜像（apk 源已指 USTC），约 2 分钟一个镜像。详见 [`build.md`](build.md)。
- 改 overlay 必须重建镜像：debugfs 对 8GiB 的 metadata_csum ext4 打不开 rw。
- QEMU 串口日志在后台跑时的坑：镜像被 QEMU 以 RW 打开，重复启动前要清掉占用进程；
  判断占用者看 `/proc/*/fd`，不要用 `pgrep -f`。
- GUI 设备的 QEMU 参数：桌面 boot 需带 virtio keyboard/mouse/gpu 设备，否则没有
  input/gpu class device。
- 宿主噪音：宿主 systemd-udevd 偶发刷 `/sys/.../uevent: Permission denied`，与 A20OS 无关。

## 四、Backspace 失效（实测结论：内核输入路径已排除，故障在合成器/终端一层）

XFCE 桌面里 `xfce4-terminal` 按 Backspace 什么都不发生，连 `^H`/`^?` 之类的可见字符都没有。
而内核输入路径是对的，这是本节的关键结论，且是实测不是推断。在 labwc 启动之前把
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

端到端复现同样清楚：在终端里注入 `echo ab<Backspace>c<Enter>`，屏幕上是 `a20os# echo abc`
且输出 `abc`。letters/space/Enter 都到位，`b` 没被删掉，说明那个字节根本没到 tty；
否则 `stty erase = ^?` 的行规一定会删掉它。pty 侧的 ERASE 是实现了的：
`kernel/drivers/char/pty.c` 的 `pty_input_byte_locked()` 里 `ch == c_cc[PTY_CC_VERASE]`
会 `canon_len--` 并按 `ECHOE` 回写 `"\b \b"`；客体里 `stty -a` 报 `erase = ^?`。

客体 xkb 数据也是完整的：`keycodes/evdev` 有 `<BKSP> = 22;`、`symbols/pc` 有
`key <BKSP> {[ BackSpace, BackSpace ]};`、`rules/evdev` 存在；labwc 日志打
`[../src/config/keybind.c:136] Found layout English (US)`，说明布局解析成功。已排除的是
labwc 自己吞键（`root/.config/labwc/rc.xml` 只绑了 `W-Return`/`W-d`/`W-f`/`W-q`/
`W-Escape`，没有 BackSpace），以及 `xkbcomp` 的
`Unsupported maximum keycode 708, clipping` 警告，那是正常现象（`keycodes/evdev`
本就声明到 708 而文件头 `maximum = 255`，任何发行版都有这条）。

判别实验（已做）：在非 VTE 的 Wayland 客户端里试同一个键，`xfce4-appfinder`
的搜索框里打 `ab<Backspace>c`，框里显示 `ac`（`b` 被删掉了）；而同一段按键序列在
`xfce4-terminal` 里留下 `abc`。**即 libinput/labwc/xkb keymap 都是好的，问题只在 VTE。**

根因已定位并修复：`kernel/drivers/char/pty.c` 的 `pty_slave_ioctl()` 实现了
`TCGETS`/`TCSETS`，但 `pty_master_ioctl()` 没有，会走到 `return -ENOTTY`。Linux 上 pty
两端共享同一份 termios，而 VTE 系终端（xfce4-terminal）正是在 master fd 上调
`tcgetattr()` 来解析它 `auto` 的 Backspace 绑定；拿到 `ENOTTY` 就学不到 erase 字符
（客体里其实是 `^?`），于是 Backspace 一个字节都不发，与「字母/回车正常、只有 Backspace
毫无反应」完全吻合。已修：给 `pty_master_ioctl()` 补上 `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`，
与 slave 共用 `g_ptys[idx].termios`。实测同一段按键序列从 `a20os# echo abc`（`abc`）变成
`a20os# echo ac`（`ac`）。

顺带纠正一条：先前怀疑的 `input_mux` 伪造 `EVIOCGBIT(EV_KEY)`（`k = 1..0xff`）不是本例的
原因，同一设备上非 VTE 客户端的 Backspace 正常。该伪造仍不干净，但与本 bug 无关。
