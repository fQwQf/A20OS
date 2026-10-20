# java 退出码 255 / mpv 偶发崩溃

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[DESKTOP-03](../desktop.md)。

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
  | v0 | `exit_group(0)` | code=0 | code=0 |
  | v1 | `exit_group(42)` | code=42 | code=42 |
  | v2 | 3 个自旋线程 + 主线程 `exit_group(0)`（JVM 的退出模式） | code=0 | code=0 |
  | v3 | 3 个自旋线程 + 非 leader 工作线程 `exit_group(42)` | code=42 | **status=0xb、sig=11(SIGSEGV)、exited=0** |
  | v4 | `exit_group(255)` | code=255 | code=255 |
  | v5 | `exit_group(-1)` | code=255 | **status=0x1、sig=1** |
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

  - 负载确实复现了目标 bug，签名与上面记的逐字节一致：
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

  - slab 检测器全程没有命中（80 次迭代，无 `[SLAB BUG]`、无 `slab_validate_sp` panic）。这类损坏
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
  - `control` 轮（子进程先写该页、真正破坏 COW 变成私有）：客体也是 clean
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
    `copy_to_user()`，说明核心里正确写法是有的，出问题的两处（cleartid 与这里）都是自己手写。
  - 也顺带澄清语义边界：对 MAP_SHARED 页裸写帧是对的（大家都映射同一帧，本就该一起改），
    错的只是 COW-private 的情况；`user_resolve_leaf()` 两种情形都能正确处理。

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
  - `pi-control` 轮（子进程先脏了该页、COW 已由子进程自己破坏）客体也是 clean
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

  **已按上述方案修复并验证**：

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

- **【第三处 + 第四处同类缺陷】**信号投递路径也在「拿裸帧重新映射」；另有影子栈 token 一处

  沿同一 bug 类继续审计 `pt_translate` / `pfn_to_virt` 的用法，又找到两处：

  - `kernel/proc/signal.c` 的 `signal_make_page_exec()`（28-54 行；唯一调用点 926 行）是
    信号投递路径：为了让栈上的 sigreturn trampoline 可执行，它 `pt_translate` 取出物理帧，
    然后 `pt_unmap()` + `pt_map(同一个 pa, ...writable_dirty_exec...)`，即把同一个帧重新映射成
    可写可执行。要注意信号帧本身是用 `copy_to_user()` 写的（915/918 行），所以帧写是安全的；
    危险的是这个重映射：若该页此刻仍与另一个任务 COW 共享，这一步会让本任务拿到
    共享帧的可写别名，此后本任务的任何写入都会改到对方那一份 （错误）。
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

  **更正（同一轮验证的结果）**：修好之后，用同一套症状负载（60× `java -version` +
  120× `mpv --vo=null`）在修复后的内核上再跑一遍，症状依旧复现：

  ```
  SIGSEGV: pid=925 code=13 sepc=0x638a6320 stval=0x8
  FATAL:   pid=925 signal=11 abi=0 pc=0x638a6320 sp=0x77dbbc40 comm=lua/console path=/extra/usr/bin/mpv
  ```

  上面那句「更有可能解释那几条偶发崩溃」是过度推断，在此撤回：信号这一处确实是同类缺陷、也确实该修
  （已修），但它不是那些偶发崩溃的原因。

  - 同一轮的回归探针全部干净：`ctidtest` 5/5、`cowtest` 5/5、`pitest` 5/5，
    说明这一轮内核改动没有破坏 COW / cleartid / futex 这几条路径。
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
    这是一个 glibc 构建的库（`ld-linux-x86-64.so.2` / `libc.so.6`），不是 musl 的 ×；
    而 `libstdc++.so.6` 出现在 `NEEDED` 里 ⇒ 抛 `std::system_error` 的 C++ 代码就在这个库内部。
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
  完整过程与证据见 [minecraft.md](../../../distro/minecraft.md)。
