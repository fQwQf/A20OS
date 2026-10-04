# 扩展这条通道：新的 ABI、新的翻译器形态

面向要**改动**这条通道形状的人（新增一种 ABI、接入非包装器型翻译器）。只想挂一个现成的翻译器请读 [02-integration.md](02-integration.md)，只想排查请读 [01-usage.md](01-usage.md)。

这一篇回答两个问题：

1. A20OS 将来要做 **native ABI 的翻译器**（qemu-user 做不到），需要改哪里？
2. Linux 的 `binfmt_misc` 在这件事上有哪些值得借鉴、哪些不该抄？

## 1. 三条轴：文件是什么、谁接得住、怎么调

把三件事分开，是这个特性能被扩展而不用重写的全部理由。把它们并成一件的失败模式是**静默**的：镜像被送给一个装不下它的翻译器，guest 看到的是崩溃而不是 `ENOEXEC`。

| 轴 | 问题 | 谁回答 | 可配置吗 |
|---|---|---|---|
| **match** | 这个文件声称自己是什么？ | `elf_probe_foreign()`（`kernel/mm/elf.c`） | 否。`e_machine` 与 ABI 都从字节里读：ABI 由 `PT_A20_START_INFO` 的存在决定 |
| **key** | 谁接得住它？ | `xlator_lookup()`（`kernel/proc/xlator.c`） | 是。查的是**配置表**，不是编译期名单 |
| **invoke** | 那个翻译器怎么被调用？ | `a20.xlator.<guest>[.argv|.env]` 启动键，缺省回注册表 | 是 |

一句话：**文件说自己是什么，配置说谁接得住。**

`match` 不可配置是刻意的。一个文件对自己的身份撒谎，那是一个坏文件，不是一个受支持的 guest。反过来，「这个文件格式合法吗」是内核本来就该回答的问题（loader 已经在回答它：`PT_A20_START_INFO` 会让 `elf_load_info_t.is_native_abi` 置位），所以这个判定写在 `mm/elf.c` 而不是 `proc/xlator.c`——**翻译策略不进 loader**。

`key` 是 `(e_machine, ABI)` 这个**二元组**，不是 `e_machine`。原因见下一节。

## 2. 为什么二元组是必需的

A20OS 有两种 ABI，两种都是本机 machine 的 ELF64：

| | Linux ABI | A20 native ABI |
|---|---|---|
| 判别 | 没有 `PT_A20_START_INFO` | 有 `PT_A20_START_INFO`（`0x6a20a200`） |
| 启动现场 | `argc/argv/envp/auxv` | `a20_start_info_t`（`kernel/include/ipc/start_info.h`），**外加一组 `a20_handle_t`**：`root_dir`、`cwd_dir`、三个 stdio 句柄、`self_task`、`main_thread`、`default_event_queue`、`service_registry` |
| 系统调用面 | Linux ABI（`kernel/abi/linux/`） | A20 原语（channel / EventQ / handle） |
| 谁跑得了 | qemu-user | 目前**没有任何翻译器** |

所以 `e_machine` 单独不足以当键：同一台机器上，x86_64/Linux 与 x86_64/native 的键**在 `e_machine` 上无法区分**，而能跑它们的翻译器**是互斥的**。

这不是理论上的洁癖。实测（riscv64 宿主，配了 `a20.xlator.x86_64=/bin/qemu-x86_64`，喂一个 `e_machine=62` 且带 `PT_A20_START_INFO` 的 120 字节合法 ELF）：

```
# 键只按 e_machine 匹配时：
[XLATOR] pid=14 execve /tmp/xlate_native.elf (e_machine=62 abi=native) → /bin/qemu-x86_64  argv="-0 @A @P @*"
qemu-x86_64: /tmp/xlate_native.elf: Unable to find a guest_base to satisfy all guest address mapping requirements
XLATE_EXEC: native FAIL: expected ENOEXEC, got no error (exit 1)

# 键按 (e_machine, ABI) 匹配时（现状）：
XLATE_EXEC: native PASS: ENOEXEC
```

qemu-user 没有当场崩，它给了句抱怨然后退出 1。但 `execve` 的调用方拿到的是「guest 退出码 1」，不是 `ENOEXEC`——一次配置不匹配被报告成了 guest 的行为。`xlate_exec native` 就是为了钉住这件事，`smoke-exec-xlator` 与 `smoke-exec-xlator-shim` 都跑它。

## 3. 加一个 native-ABI 翻译器：需要改什么

**一句话：注册表加一行，命令行加一个键，内核代码不动。** 下面是逐步。

### 第 1 步：想清楚 native ABI 的翻译器是什么形态

这一步决定后面所有事，而仓里给不出答案，只能给出判据。

native ABI 的 guest 需要 `a20_start_info_t` 里那组 **handle**，而 handle 不是 fd。它需要的东西和 Rosetta 处理 Mach-O 的方式**不是一类**：Rosetta 把 guest 的 guest-thread 映射成宿主的一个 pthread，然后按需翻译 guest 的 fd 与 Mach port；A20 的 handle 需要同样的映射，但语义是 channel/EventQ 的能力端点（见 [../hybrid-kernel/05-idl-and-personality.md](../hybrid-kernel/05-idl-and-personality.md)）。

于是判据是：

- **翻译器本身是 A20OS 原生程序**（能拿 `a20_handle_t`、能调 A20 原语）——必须是，否则它拿不到 `root_dir` / `self_task` / `service_registry`；
- **它把外来架构的指令翻译成宿主指令并在自己的地址空间里跑**（Rosetta 形状，包装器型）——那么第 2–4 步就够了；
- **它跑一遍产出产物再 exec 产物**（Prism 形状，编译器型）——那么还需要第 5 节那一整块机制。

一个常见的错误路线是「移植 qemu 加个 linux-user 变体」。**qemu 的 linux-user 模式整个建立在「guest 是 Linux 进程」之上**：它的目标机模型就是 Linux 的 syscall ABI，A20OS 没有 Linux 进程概念可给它。所以复用 qemu 的可行形态是把它当**动态翻译引擎**（TCG）而不是当 linux-user 仿真器——这仍然是一条实打实的工程，但它是 qemu 的一个*移植*，不是 exec-xlator 的一个配置项。

### 第 2 步：注册表加一行

`kernel/proc/xlator_guests.def`：

```
XLATOR_GUEST(<name>, <e_machine>, XLATOR_ABI_NATIVE, <argv_template>)
```

与 Linux ABI 那几行并存，两者**不冲突**：键是二元组，`x86_64/linux` 与 `x86_64/native` 是两行。

三条约束（`make check-xlator-guests` 全部会断言）：

| 列 | 要求 |
|---|---|
| `<name>` | 合法标识符片段、不能含 `.`；同时用作 `a20.xlator.<name>.<abi>` 这个键的主体、`tools/targets-xlator.mk` 里 shell 变量的后缀、以及 `/proc/a20/xlator` 的行首 |
| `<e_machine>` | 十进制数，必须与 `kernel/include/mm/elf.h` 的 `EM_*` 一致 |
| ABI | 只能是 `XLATOR_ABI_LINUX` 或 `XLATOR_ABI_NATIVE`，且后者必须对应 `elf.h` 里真实的 `ELF_ABI_*` |
| `<argv_template>` | 规则与 Linux ABI 那几行完全一样（见 [01-usage.md](01-usage.md#4-argv-模板语法)） |

**名字可以重复，键不能。** 这是本设计里唯一一处「同名不同行」被明确允许的地方，也是最容易想反的一处：查找键是 `(e_machine, ABI)` 二元组，所以同一个架构名注册两个 ABI 是**正常**的，不是不小心写重了。真正必须唯一的也是这个二元组——同一个 `(名字, ABI)` 出现两次，其中一行永远匹配不上，而且不会有任何日志点名它。

启动键因此写成 `a20.xlator.<name>.<abi>`，裸的 `a20.xlator.<name>` 是 `(名字, linux)` 的简写：

```
a20.xlator.x86_64=/bin/qemu-x86_64                 # = a20.xlator.x86_64.linux=…
a20.xlator.x86_64.native=/bin/prism-x86_64          # 同一架构的第二个 ABI
```

裸名解析成 linux 而不是「第一个注册的 ABI」是刻意的：它让现存的每一条命令行一个字都不用改，而通配语义意味着某天加第二行时，所有旧配置会**默默地**改指到另一行去。

> 这条约定不是推演出来的，是实测过的：临时加一行 `XLATOR_GUEST(x86_64, 62, XLATOR_ABI_NATIVE, "@P @*")` 并配上 `a20.xlator.x86_64.native=`，启动日志如期打出两行互不覆盖的
> ```
> [XLATOR] x86_64 (e_machine=62 abi=linux)  → /bin/qemu-x86_64  argv="-0 @A @P @*"
> [XLATOR] x86_64 (e_machine=62 abi=native) → /bin/qemu-x86_64  argv="@P @*"
> ```
> 两行各自拿到自己那份模板，`/tmp/xlate_native.elf` 被转发到 native 行而不是 linux 行。零内核代码改动。

### 第 3 步：`tools/targets-xlator.mk` 加交叉编译器

```
XLATOR_GUEST_CC_<name> := <交叉编译器>
```

CC 行是按**架构名**索引的，不是按 `(名字, ABI)`：同一个架构的两个 ABI 行用同一把交叉编译器。所以两行 ABI 共用一个名字时，`XLATOR_GUEST_CC_x86_64` 一行就够了，门禁的反向检查也按名字集合比对，不会因为多了一行而误报。

> 这一步**还没做**：目前一个 native 行的翻译器不是交叉编译出来的（`prism` 一类的形态尚未选定），而门禁要求「注册表里有、CC 表里没有」就 FAIL。要动的是把 CC 契约放宽成「有 probe 才要求 CC 行」，而不是给一把用不到的编译器凑数。写下它是为了让做这件事的人知道会遇到什么。

### 第 4 步：配置与验证

```
a20.xlator=1
a20.xlator.x86_64=<linux 翻译器路径>          # 裸名 = .linux
a20.xlator.x86_64.native=<native 翻译器路径>
```

验证形状仍然用 shim，不要用真翻译器（理由见 [02-integration.md](02-integration.md#第-3-步用-shim-验证形状不要用真翻译器验证)）。判据：

- `cat /proc/a20/xlator` 里两行都在，`abi=` 分别是 `linux` 与 `native`；
- `xlate_exec stage 62 /tmp/g` 造出来的**没有** `PT_A20_START_INFO` 的文件走 Linux 行；
- `xlate_exec native` 造的**有** `PT_A20_START_INFO` 的文件走 native 行；
- 两行都 `[XLATE_EXEC: ... PASS: exit=…]`。

**`xlate_exec native` 的期望会翻转。** 今天这条 fixture 断言的是「必须是 `ENOEXEC`」，因为没有任何一行 native 翻译器；一旦你加了那一行，同一个断言就该翻转成「必须被转发」。这不是 fixture 坏了，是它测的东西变了——它测的正是「native 镜像不会被误送给 linux 翻译器」，而加了 native 行之后，正确行为变成了「被送给 native 翻译器」。改的时候记得把 `smoke-exec-xlator` 的 `expect` 与 `smoke-exec-xlator-shim` 那两条 `forbid`（`XLATE_SHIM: \S*xlate_native`、`[XLATOR] pid=\d+ execve /tmp/xlate_native\.elf`）一起翻过来，否则门禁会红，而红的原因不是内核坏了。

注意最后一条：**现在 native 行必然跑不通**，因为没有 native 翻译器。这一步在翻译器存在之前只能验证到「查得到、转发得出去、argv 形状对」，跑通 guest 是翻译器自己的事。上面第 2 步那个实测日志正是这个状态：转发发生了，`qemu-x86_64` 抱怨 `Unable to find a guest_base`——它只实现 Linux ABI，这恰恰证明请求确实走到了 native 行，而不是掉回 linux 行。

### 第 5 步（只有编译器型翻译器才需要）

见第 5 节。

## 4. 为什么现在**不**预留一张 vtable

一个显然的「预留扩展性」做法是给每个 guest 加一个 `struct xlator_ops` vtable，里面放 `prepare` / `invoke` 两个函数指针，现在全填 NULL。这个仓已经因为同样的理由拒绝过一次：`argv0_flag` 那一列被模板取代了，因为按 guest 逐个加一个「调用约定」字段，等于把「翻译器长什么样」硬编码进表结构。模板做到了同一件事且没有表结构改动。

现在**剩下的**那条轴确实是模板覆盖不到的：

| 形态 | 模板够吗 |
|---|---|
| 包装器型，`[argv0, path, args...]` 任意拼法 | 够 |
| 编译器型（Prism）：跑一遍，**产出**一个镜像，然后 exec 那个镜像 | **不够**。这不是 argv 的问题：内核现在从不等待另一个程序的产出 |

但那条轴要的东西不是「多一个函数指针」，而是**一整段机制**：谁跑编译器、产物放哪、怎么命名与淘汰、产物是宿主 ELF 但要可写可执行（`CONFIG_XLATOR` 现在给的是 per-task 的 W^X 放行，不是文件权限）、以及失败时该报什么错（`ENOEXEC` 已经明确表示「这不是能跑的东西」，拿来表示「翻译器编译失败」是撒谎）。第 5 节展开。

结论：等真有第二种执行形态时再建 vtable，而且它会是**每个 guest 一个操作**（因为「编译并缓存」这件事按 guest 语义不同），不是通道级的开关。现在建一张全 NULL 的表，只是把一个还没决定的设计提前焊进类型里。

## 5. 编译器型（AOT）翻译器需要的那一整块

[02-integration.md](02-integration.md#5-aot-型翻译器prism-一类尚不支持) 从接入者视角列了四条。这里补上**它在接口上落在哪里**，因为这是扩展这篇文档的人真正要回答的问题。

今天 exec 路径的形状是：

```
elf_load(镜像) --ENOEXEC--> exec_try_script? --> exec_try_translator? --> ENOEXEC
                                                          │
                                    改写 bprm（路径 + argv + env），EXEC_RETRY
```

AOT 需要插在**改写 bprm 之前**，而且不是一次改写而是一段：

```
elf_load(镜像) --ENOEXEC--> exec_try_script? --> exec_try_translator? --> ENOEXEC
                                                          │
                            ┌─────────────────────────────┴──────────────────────────┐
                            │ 包装器（今天）                                          │ 编译器型（缺失）
                            │   bprm.path  = <已配置路径>                            │   fork/exec <编译器>，同步等它
                            │   bprm.argv = 模板展开                                  │   读它写出的产物路径
                            │   bprm.env  = .env 注入                                │   校验产物是宿主 ELF 且可执行
                            │   → EXEC_RETRY                                         │   bprm.path = 产物；bprm.argv = 原样
                            │                                                       │   → EXEC_RETRY（且不再重复编译）
                            └───────────────────────────────────────────────────────┘
```

四个接口点，各自不是配置项：

1. **同步点。** exec 热路径上要等另一个进程。目前 `execve` 路径里没有任何「等待子进程」的位置，`wait4` 的一个次序疑点（见 [03-internals.md](03-internals.md#10-已知代价与未做的事)）说明这块的次序语义本身还没被完全钉住。
2. **产物身份。** 名字必须由「镜像路径 + 内容」派生，否则要么每次重编要么撞名；还要有淘汰与配额。这是内核要管理的缓存，cmdline 上的一个路径字符串表达不了。
3. **产物权限。** 它是宿主 ELF，所以 `elf_load` 吃它是顺的；但它必须可写可执行，而 `CONFIG_XLATOR` 给的是 **per-task W^X 放行**（`kernel/mm/wx.c` 里 `task_t.xlator_host`），不是文件权限。产物需要自己的权限处理路径。
4. **错误契约。** 编译器崩溃 / 缓存写满 / 目标不支持，三种都要新错误，不能复用 `ENOEXEC`。

所以它应该长成**注册表里一个新的 class**（`XLATOR_ABI_*` 的邻居），配合自己的 exec 阶段——不是把 `@P @*` 硬拗出第三种含义。这一步做完之前，不要预留一个假的 `aot` 枚举值。

## 6. 从 Linux `binfmt_misc` 借鉴了什么

Linux 的 `binfmt_misc`（`fs/binfmt_misc.c` 与 `Documentation/admin-guide/binfmt-misc.rst`，torvalds/linux master）做的是同一件事的通用版：按文件特征决定用哪个解释器。本节只写**我们采纳的**与**我们决定不抄的**，理由都写在代码或本文的对应位置。

### 值得借鉴的

| binfmt_misc 的做法 | A20OS 的对应 | 为什么 |
|---|---|---|
| **注册**（往 `/proc/sys/fs/binfmt_misc/register` 写一条 `:name:type:offset:magic:mask:interpreter:flags`） | 启动键 `a20.xlator.<guest>=…` | 两者都是「按特征登记，而不是编译期枚举」。差别是我们只在启动时收，运行时不可改 |
| **`load` 失败即失败，`-ENOEXEC` 才落到后续格式** | `exec_try_translator()` 返回「不适用」时落到 `exec_try_script` 之后的普通 `ENOEXEC` | 同一份失败契约：我们把「不是我的事」和「是我的事但坏了」分开，后者是一个具名的启动告警 |
| **`F`（fix binary）：注册时就把解释器打开** | 路径在 `xlator_config_init()` 解析一次，此后永不重解析 | 这是同一个想法的两次实现。binfmt 需要这个 flag 是因为它有 mount namespace；A20OS 没有 namespace，所以直接不做成开关 |
| **`max_binfmt_misc_interpreters` sysctl：给「预先打开的解释器」一个上限** | 无对应物 | 这条值得记住但**当前不适用**：它防的是「非特权用户在 namespace 里预先打开文件、把 inode 钉住」。A20OS 没有这个攻击面，所以加一个上限就是没有理由的复杂度 |
| **`L`（loader）只给**本机架构**的 ELF 用** | 键是 `(e_machine, ABI)` | 同一个区分。binfmt 把它做成一个 flag，因为它要同时服务「本机解释器」和「外来仿真」两类 handler；我们的表天生分开，于是直接放进键里 |
| **`match` 与 `load` 都可交给 BPF（`CONFIG_BINFMT_MISC_BPF`）** | 无对应物 | 记下来是因为它是最新的形态：**匹配**与**调用**都成了可编程的。也就是本节说的第三条轴在 Linux 上被推到了极致 |

### 明确没有抄的：调用模板

这是最值得记的一条，而且是**反直觉**的一条。

Linux **删掉了** binfmt_misc 的调用模板。旧接口里解释器可以带 `%b` / `%O` / `%P` / `%p` / `%N` 之类的替换记号；master 上 `build_interp_argv()` 换成了一个**固定形状**：

```c
/* fs/binfmt_misc.c, master: 固定形状，[interpreter, (可选 staged arg), binary, args...] */
```

调用者 `argv[0]` 能不能留下，由一个正交的 `P`（preserve argv[0]）flag 决定；`O`（open-binary，给解释器传已打开的 fd）、`C`（credentials）、`T`（transparent）、`F`、`L` 也都变成了 flag。

也就是说，Linux 的方向是**从「内核提供一个自由格式的调用字符串」退回到「一个固定形状 + 少量正交 flag」**。

我们为什么还留着 `@A/@P/@*`：

- binfmt 的固定形状表达不了 `--argv0=@A @P @*`（选项带紧贴的值），也表达不了 `@P @*`（不要 `argv[0]` 覆盖）与 `-0 @A @P @*`（要一个选项）之间的差别——它只有一个 `P`。
- 我们需要的形状恰好就是「选项位置与形态随翻译器而变」这一族，模板是它们的最小表达。
- 我们的模板**只有一个调用点**（`exec_try_translator()`），语法在启动时就地校验，未知记号与缺少 `@P` 都被拒；binfmt 的模板语法面向的是**用户态任意写入**的注册串，攻击面大得多。两条路线的风险不在一个量级。

但这条教训本身成立：**内核提供的自由格式调用字符串是一种负债**。所以我们的模板被刻意做窄——三个记号、有上限、启动时解析一次、规范化成一种拼写，且**永远不给它第三种含义**（AOT 需要的是机制，见第 5 节）。

### 一条结构性差异

binfmt_misc 的 handler 可以注册、注销、注册成禁用（`D` flag），运行时可变。A20OS 的注册表是编译期常量、配置是启动期常量、运行期只有**通道级开关**。这是刻意的收窄：通道已经有两个正交的控制面（配置 + 开关），再加一层运行期注册表会让「为什么这个 exec 没有被转发」变成一个需要四个变量才能回答的问题。逐 guest 的运行期开关没有做，理由见 [01-usage.md](01-usage.md#9-已知边界)。