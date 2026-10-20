# x86_64 桌面组件随机 SIGSEGV

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[DESKTOP-08](../desktop.md)。

xfdesktop/panel/thunar/gst/udevd/dbus-daemon/xfsettingsd 随机野指针崩溃，只在 x86_64 上出现。
原先记的解释是「x86_64 要在上下文切换与信号帧保存/恢复 FPU/SSE，缺这层时被抢占线程的 XMM 会被
下一个任务覆盖」——**这条修法已经在树上（`11d51ae2` 上下文切换与信号帧、`4cc26164` fork 继承、
`b6d7678e` 信号栈 16 对齐），但症状照旧**，所以解释不成立，条目挪到这里。

同日三次 `a20 run xfce-x86_64` 各打中不同的进程，说明崩的不是某个程序而是「随机挑一个用户态进程」：

| 轮次 | 受害者 | 内核侧签名 |
|---|---|---|
| fix3 | `udevd` / `xfce4-panel` / 监控 shell | `code=13 stval=0x10`、`stval=0x18`（**rax=0**，NULL+偏移）；`code=12 stval=0x0` |
| fix4 | `udevd` | `code=13 sepc=0x4270a51b stval=0x18` |
| fix5 | **`dbus-daemon`** / `xfsettingsd` | `code=13 sepc=0x11bc39665 stval=0x12bc94aa0`（**规范但未映射**的堆地址） |
| fix7 | `xfce4-panel` 内 GDBus 线程 | `code=1` #GP，`pc=0x430f603d`，`comm=gdbus path=.../xfce4-panel` |
| fix8 | `xfce4-panel` / **`dbus-daemon`** / `xfsettingsd` / 监控 shell | 见下「寄存器是脏的、代码是好的」 |
| 历史 | `tumblerd` | `code=1`(#GP, 非规范地址)，`insn@sepc=0x48b60ff4` |

三种形状指向同一个东西——「某个指针字段变成 NULL、野值或已释放的地址」：NULL+偏移是空指针解引用，
#GP 是非规范基址，`0x12bc94aa0` 是看起来合理但没映射。riscv64 同期连跑两轮
`a20 run xfce-riscv64`、含 probe 全程，**0 FATAL / 0 PANIC**，所以是 x86_64 侧的问题，不是桌面包本身。

`dbus-daemon` 是这一类里最致命的一个：它一死 `dbus-run-session` 就报
`dbus-daemon terminated by signal 11` 并带走 labwc，panel / tumblerd / xfsettingsd / xfdesktop
随后各自打印 `Name ... lost` / `The connection is closed` 退出，串口从此安静——**内核和客户机都还
健康，桌面却没了**。所以 `start-xfce4-session` 不再 `exec` 一次就完事，改成有界重启整个会话
（最多 3 次，中间隔 2s），把「一次用户态崩溃 = 登录会话结束」降级成可恢复事件。

**fix8 补充：光等 `dbus-run-session` 退出不够。** 该轮 `dbus-daemon` 再次 SIGSEGV，串口打印
`dbus-run-session: dbus-daemon terminated by signal 11` 之后**再没有任何 `[SESSION]` 输出**，而截图上
壁纸仍在（labwc 还活着），即 `dbus-run-session` 卡在 wait 里、原来的 `cmd || true` 循环永远走不到
attempt 2。看门狗因此改成外置的：会话放后台跑，每 30s 查一次 `pidof dbus-daemon`，连续两次
（间隔 10s）查不到就主动 `kill` 掉会话重启，最多 3 次。启动后前 15s 不判活，避免把还在起的总线
误判成死的。

### fix8 新增诊断：pc 都落在合法可执行文本里，脏的是寄存器

`kernel/core/trap.c` 的 `CAUSE_INSN_FAULT/LOAD/STORE` 分支现在也调用 `dump_fault_pte(cur, sepc)`
（原先只有 SIGSEGV 分支打），于是 #GP 也会打印覆盖 pc 的 VMA 区间/flags/**文件路径**、PTE 与页内容。
读 dump 时注意寄存器名是 RISC-V 口音：`a0..a5` = rdi/rsi/rdx/r10/r8/r9，**`a7` 就是 rax**
（`TRAP_CTX_SYSCALL_NUM`），`ra`=rbp、`tp`=user_fs_base；x86 额外那几行才给 rbx/r12–r15/r10/r11
（映射见 `kernel/arch/x86_64/include/trap_frame.h` 的 `arch_trap_ctx_reg`）。

fix8 一轮里四次崩溃，pc **全部**在合法、file-backed、可执行的库文本中：

| 受害者 | pc 所在映射 | 出错指令与寄存器 |
|---|---|---|
| `xfsettingsd` | `libgobject-2.0.so.0` | `mov (%rax),%rax`，**rax=0xaaaaaaaaaaaaaaaa**（非规范）→ #GP |
| 监控 shell | `ld-musl-x86_64.so.1` | `movzbl (%rax),%edx`，**rax=0x3d79726575000072**（ASCII 片段）→ #GP |
| `xfce4-panel` | `ld-musl-x86_64.so.1` | `mov %r11,(%rsi)`，**rsi=0x9** → store #PF 到 0x9（`stval=0x9` 与 a1=rsi 自洽） |
| `dbus-daemon` | `ld-musl-x86_64.so.1` | pc 落在一段合法序言**前 2 字节**（取指得 `f4`=HLT）→ #GP |

这修正了此前「跳进堆里执行」的推断：**代码本身是好的，坏的是寄存器里的值**（以及一次明显错位的
落点），即用户态内存中的指针字段被写坏。riscv64 同期连跑 0 FATAL/0 PANIC，仍是 x86_64 独有。
另外 fix7 把取指字 `f4 8b 48 18`（以及历史那条 `f4 6f b6 48`）在全部 517 个 x86_64 apk 与
`build/repo` 里全文搜索，故障进程相关的包中**没有命中**，说明至少有一次落点字节不属于任何已安装
ELF（`build/repo` 的 busybox 系列有一批巧合命中，但都不在故障进程里）。

面板不绘制这条也因此有了直接死因：x86 上 `xfce4-panel` 起来就被 ld-musl 里那条 `mov %r11,(%rsi)`
打成 SIGSEGV（进程死掉，面板窗口自然不映射），而 riscv64 同样的包一路正常。此前加的 xfconfd
round-trip 闸门本身是生效的——fix7 探针回的是 `xfconf probe query=p=6;x=0;y=0`，即总线和配置服务
都应答了，剩下的拦路就是这次崩溃。

### fix9/fix10：崩溃点反汇编到了具体函数，#GP 的机制也定了

**先说结果。** fix9 冷启动后顶栏与壁纸同屏保持 18 分钟：顶部 0–26 行是 `#353535`
（1024×768 截图里 29496 个像素，正好是面板条），27 行往下是壁纸 `(0,144,188)`，
`a20: xfconf probe query=p=6;x=0;y=0` 与 `pids xfconfd=81 79 78 71 panel=114 109 101 100 99 96`
都在，0 PANIC / 0 `[YIELD-BUG]`，全程没有一行 `[SESSION]`——即外置看门狗把健康会话
判成活的、没有误杀（这正是它要保证的事；fix8 里旧的 `cmd || true` 循环则一次都没走到
attempt 2）。唯一一例 FATAL 仍是 `xfsettingsd`，与 fix8 同一 pc 逐字节复现。

**崩溃点。** 把 `libgobject-2.0.so.0` 从 `build/cache/apk/x86_64/glib-2.86.3-r0.*.apk`
抽出来（`vma off=0xc000` + `0x2e2bd` = 文件偏移/vaddr `0x3a2bd`）反汇编，pc 正落在：

```
3a2b0 <g_type_check_instance_is_fundamentally_a>:
  3a2b0  test %rdi,%rdi          ; instance != NULL
  3a2b5  mov  (%rdi),%rax        ; rax = instance->g_class
  3a2b8  test %rax,%rax
  3a2bd  mov  (%rax),%rax        ; ← #GP 在这里
```

调用本身是合法的：`rsi=0x50` 就是 `G_TYPE_OBJECT`（fundamental type `20<<2`），
`rdi=0x1399d31a0` 是看着正常的堆指针（`rbx` 里还留着同一份副本）。所以结论只有一种：
**这个 GObject 的 `g_class` 字段读出来是 `0xaaaaaaaaaaaaaaaa`。**

**为什么是 #GP 不是 #PF。** `0xaaaaaaaaaaaaaaaa` 的 63:48 位不是 47 位的符号扩展，
是非规范地址，CPU 直接给 #GP（`code=1`），不产生 CR2，所以 `stval` 还是上一次残留的
旧值（`stval=0x13785a610` 是个堆地址，纯属巧合）。这也就解释了历史那条 `tumblerd` 的
`code=1`（#GP, 非规范地址）为何同款签名——**这一类崩溃的统一形状是「指针字段被写成
非规范值」**，而 `code=13 stval=0x10/0x18`（NULL+偏移）与 `stval=0x12bc94aa0`
（规范但没映射）是同一根因的另外两种表现。

**排除掉的解释。**
- 物理页没问题：`pa=0x172e8f2bd` 落在 `[RAM] usable 0x100000000..0x180000000`
  （`-m 4G` 的高半区，QEMU 低半区只到 `0x7ffd9000`）之内，覆盖 pc 的 `libgobject`
  文本页是正常 file-backed 映射，不是「页表指向不存在的物理页」。
- `0xaaaaaaaaaaaaaaaa` 不是谁加载进来的常量：全树（`kernel/`、`packages/`、
  `third_party/`、构建 flag）没有这个值，也没有 `-ftrivial-auto-var-init`；
  517 个 x86_64 apk 里只有 `libgcrypt.so.20.6.0`（一段栈上位掩码表：
  `movabs $0xaaaaaaaaaaaaaaaa,%rax` 后 `mov %rax,0x8(%rsp)`，同处还有
  `0x5555…/0x3333…/0xcccc…` 一族）与 `libLLVM.so.21.1` 带这条立即数，
  另有 22 个文件含裸 8×`0xaa`（字体、icu 数据、gstreamer 插件等），都不在故障进程里。
- 因此更像是「对象首字被写成了这个位型」，而不是常量泄漏。为此 #GP 分支又加了一层
  dump：`ADE-RDI/RBX/RSI/RDX` 打印各基址寄存器指向对象的 64 字节窗口（`dump_user_object`）
  和它自己的 VMA/文件路径（`dump_fault_pte`），非规范值只打印不走页表；下一次崩溃就能
  直接看出坏指针落在堆、匿名区还是文件数据里。

### fix11：出错指令就是 musl 自己的 `hlt` 断言，而且崩溃后 panel 主进程还活着

**`insn@sepc=0x18488bf4` 不是「跳到野地址执行」。** fix11 的崩溃是
`ADE/ALE: pid=108 sepc=0x4182603d stval=0x15e81d16d code=1`，
`vma_file=/extra/lib/ld-musl-x86_64.so.1`（文件偏移 `0x46000 + 0x3203d = 0x4603d`）。
把 `build/tmp/ld-musl.so` 的 `0x46000–0x460d0` 反汇编出来，`0x4603d` 正是 mallocng
元数据自检里的一条 `hlt`：

```
46023: mov  -0x10(%rdi),%rax      ; rax = meta
46027: lea  -0x10(%rdi),%rcx      ; rcx = &chunk
4602b: cmp  %rcx,0x10(%rax)       ; meta 必须回指这个 chunk
46031: hlt                        ; ← fix10 落在这一步（0x10(%rax) 读到 0 → #PF 0x10）
46032: movzbl 0x20(%rax),%ecx     ; sizeclass 字节
46036: and  $0x1f,%ecx
46039: cmp  %edx,%ecx             ; 必须 >= 调用方要的档位
4603b: jge  4603e
4603d: hlt                        ; ← fix11 的 pc 正落在这里
4603e: mov  0x18(%rax),%ecx       ; 取指字 f4 8b 48 18 的后 3 字节
```

那 4 个字节 `f4 8b 48 18` 就是 `hlt; mov 0x18(%rax),%ecx`——即历史上反复出现、
first/fix1/fix7/fix11 五次同款的 `insn=0x18488bf4`，是 GCC 为 musl
`__builtin_trap()` 生成的 `hlt`，**`hlt` 在 ring 3 执行就触发 #GP（`code=1`），
`stval` 因此是上一次的残留值**。同理 fix8 那条「`dbus-daemon` pc 落在合法序言前 2
字节、取到 `f4`」也不是落点错位：`ld-musl 0x419d7 <__stack_chk_fail>: f4 c3`
（`build/tmp/ld-dbus.txt`），取到的 `f4` 就是栈保护检查失败后的 `hlt`。
**修正结论：这些崩溃是 musl/glib 的自检在已经被写坏的状态上开火，控制流自始至终
都在合法、file-backed 的文本段里**，上一节「pc 都落在合法文本里」由此从「观察」
升级成「机制」。

**坏的到底是什么。** fix11 的 ADE dump 给出了失配的具体数字：`rax=0x330ff40` 指向
brk 区里一块**真实的** meta（`meta+0x10 = 0x15e809fb0`，正好等于 `rdi-0x10`，chunk↔meta
双向自洽），它的 `*(u8*)(meta+0x20) = 0xfe`，`0xfe & 0x1f = 0x1e`，而调用方要求的
`rdx/r8 = 0x1f` → `0x1e < 0x1f` → `jge` 不成立 → `hlt`。也就是**那个字节的最低位被
清掉了**（`0xff → 0xfe` 的形状）。fix10 是同一条自检的上一步：从活的堆头
`mov -0x10(%rdi)` 读出来是 **0**（前后邻居指针都正常）→ `cmp 0x10(%rax)` 在 `0x10`
上 #PF。连同 glib 那例 `g_class=0xaaaaaaaaaaaaaaaa`，目前见到的坏法一共三种：
**整字位型、单 bit/字节被改、整字被清零**。谁写的仍未定位（x86_64 独有，riscv64
同期 0 FATAL），`kernel/mm` 里没有 32 位物理地址截断的嫌疑，内核 C 是
`-mgeneral-regs-only` 也没有 SSE 覆盖。

**崩溃之后进程没有被收掉，有界重启因此一次都没触发。** 崩溃任务 `pid=108 comm=gdbus`
其实是 panel 主进程 `94` 的一个线程：`name` 只会被 `exec`
（`kernel/proc/exec.c:991` 按 basename 设置）和 `prctl(PR_SET_NAME)`
（`kernel/abi/linux/sys_proc.c:1295`）改掉，`path=` 打的是继承来的 `exec_path`，
所以 `gdbus` 是这个线程自己 prctl 出来的名字，进程仍然是 `/extra/usr/bin/xfce4-panel`。
但崩溃之后 45 秒，探针 `a20: xfconf probe pids … panel=94` **仍然返回 94**，日志里
自始至终没有一行 `a20: xfce4-panel exited, fast deaths …`，也没有 `attempt 2/5`
（fix10 同样），而两次截图上顶栏都没画出来——即「线程崩了、线程组没退出、面板也没
画」，`start-desktop-components.sh` 的 `spawn` 只靠 `wait "$pid"` 等自己的直接子进程，
这一路完全没被触发。两条候选解释都还没验：`proc_exit_group()`
（`kernel/proc/exit.c:686`）虽然从 `self->tg_leader` 起遍历整个线程组，但循环里
`exit_pending` 已置位的任务会被跳过；或者 leader 已经是僵尸、只是被 `pidof` 列出来
而 `wait` 没被唤醒。**这也是「面板不绘制」在 fix10/fix11 上重现的直接原因**——
与 fix7 那种「起来就被打死、进程消失」不是同一种形态。
