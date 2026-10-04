# 实现与设计取舍

面向要改这段内核代码的人。**配置怎么用见 [01-usage.md](01-usage.md)，怎么接入翻译器见 [02-integration.md](02-integration.md)，怎么扩展这条通道（新增 ABI、非包装器型翻译器、binfmt_misc 的借鉴与教训）见 [04-extending.md](04-extending.md)**，这里只讲「为什么是这个形态」「代码在哪」「契约与不变量是什么」。

## 1. 代码地图

| 文件 | 角色 |
|---|---|
| `kernel/include/proc/xlator.h` | 全部 API 与契约。`CONFIG_XLATOR=0` 时提供 stub，调用点因此不必自己加 `#ifdef` |
| `kernel/proc/xlator.c` | 启动时解析 `a20.xlator*` 键；运行期开关与 `/proc` 渲染。整个文件体包在 `#ifdef CONFIG_XLATOR` 内，**不写 `#else`** |
| `kernel/proc/xlator_guests.def` | 唯一的 guest 注册表（X-macro，被 `xlator.c` `#include`，同时被 `tools/` 解析） |
| `kernel/proc/exec.c` | `exec_try_translator()`：argv 改写 + env 注入 + W^X 标记 |
| `kernel/mm/elf.c` | `elf_probe_foreign()`：只回答「这是一个格式合法的外来架构 ELF64，且它的 ABI 是什么」 |
| `kernel/include/mm/elf.h` | `elf_abi_t` / `elf_guest_key_t`：`match` 轴的结果类型，被 loader 与通道共用 |
| `kernel/mm/wx.c` | `mm_wx_filter_prot()` 里对 `task_t.xlator_host` 的放行 |
| `kernel/fs/procfs/procfs.c` | `/proc/a20/xlator` 节点的注册、0644 模式、写入处理 |
| `kernel/main.c` | `xlator_config_init()` 的调用点，紧随 `mm_wx_policy_init()` |
| `kernel/core/bootargs.c` | `arch_bootargs_get()` 弱默认（返回 NULL）与 `bootargs_get()` |
| `kernel/core/fdt_bootargs.c` | 各 FDT 架构共用的 `/chosen/bootargs` 提取器 |
| `kernel/core/uart_cmdline.c` | `uart_cmdline_read()`：`UART_CMDLINE=y` 时在串口上收一次命令行，5 秒无输入即放弃。整份文件包在 `#ifdef CONFIG_UART_CMDLINE` 内，默认关 |
| `tools/xlator_guests_audit.py` | `check-xlator-guests` 门禁的实现 |
| `user/cmds/core/xlate_shim.c` | 只打印 argv/env 的宿主替身，形状验证用 |
| `user/cmds/core/xlate_exec.c` | Linux-ABI 启动器与断言器，`smoke-exec-xlator*` 的驱动 |

注册表（`kernel/proc/xlator_guests.def`）的当前内容：

```
XLATOR_GUEST(x86_64,  62,  XLATOR_ABI_LINUX, "-0 @A @P @*")
XLATOR_GUEST(aarch64, 183, XLATOR_ABI_LINUX, "-0 @A @P @*")
```

第三列是 ABI，它是键的一半而不只是元数据：见第 4 节。

## 2. 为什么是「就地 re-exec」，不是「转发给服务」

混合内核的另一条直觉路线是把这个翻译器当用户态驱动，让内核把外来二进制经 channel 投递给它。这个形态最终没选，理由是 exec 语义与句柄投递两个具体约束。

**exec 的语义要求调用者变成目标进程。** `execve` 返回后，调用方的地址空间、凭据、pid 全部被目标镜像替换。如果外来二进制被投递给一个常驻服务进程，那么该服务必须代表调用者执行一整个程序，同时把它的每一次 syscall 都代理回内核——这就是 starnix 式的人格层，量级与本特性完全不同（见 [../hybrid-kernel/05-idl-and-personality.md](../hybrid-kernel/05-idl-and-personality.md)）。就地 re-exec 没有代理：内核改写要加载的镜像与 argv，然后走它本来就会走的 `execve` 循环。qemu-user 本身是普通的 Linux-ABI 程序，它在**自己的**地址空间里 JIT guest，guest 因此对内核完全是一个原生进程。

**re-exec 绕开了已知的句柄投递缺口。** `kernel/ipc/channel_fd.c` 在 fd 读路径上丢弃消息携带的句柄。guest 路径如果走 channel，请求与响应就得穿过这条路径，其携带的句柄语义是不完整的。走 argv 就完全不碰它。

代价要说清楚：re-exec 会重置调用方在 `execve` 之前建立的现场。这与原生 `execve` 一样，本来就是调用方的义务。

## 3. 与阶段五人格层的边界

| | exec-xlator（本特性） | Linux 人格层（阶段五） |
|---|---|---|
| guest 是 | 外来架构的**编译器产物** | 外来架构的**任意进程** |
| guest 看到的世界 | 一台完整的、语义等价的 Linux 机器（由 qemu-user 用 A20OS 的 Linux ABI 面合成） | A20OS 的原生原语（channel/EventQ/handle） |
| guest 自身的 syscall 处理 | qemu-user 拦截并翻译成 Linux ABI 调用 | 内核人格层直接实现 |
| 内核改动 | `exec.c` 一个分支 + 一个 peek 辅助 + 一个 cmdline 模块 | 需要 fd/mmap/socket/futex/epoll 的完整仿真 |

两者的关系是分工而非替代。边界的选择标准是**诚实**：在 guest 看到一台 Linux 机器的地方就声称人格层已经完成，是不成立的。

## 4. 外来架构判定：与「坏格式」严格分开，且读出的是**二元组**

外来 machine 与文件损坏原本共用同一个 `-ENOEXEC`：`kernel/mm/elf.c` 的判定返回 bool，两种情形在 `elf_load` 里坍缩成同一个错误码。直接复用它无法区分「请翻译这个」和「这个文件坏了」，误判会让坏文件被送进翻译器。

因此新增 `elf_probe_foreign(fd, &elf_guest_key_t *)`，**只在 `proc_exec` 的 ENOEXEC 分支按需调用**，不改动 `elf_load` 的返回值契约。它读头部、校验 magic/class/data/type/phentsize/phnum，然后：

- 宿主原生 machine → 返回 `-ENOEXEC`（走原路径，不算外来）；
- 其它 machine → 继续扫一遍 program header，找 `PT_A20_START_INFO`，返回 0 并输出 `(e_machine, abi)`。**是否真的可翻译由通道配置回答。**

### 为什么输出的是二元组

A20OS 的两种 ABI 对同一台机器是同一个 ELF64，唯一的区别是「有没有 `PT_A20_START_INFO`」。所以 `e_machine` 单独**不足以**当键，而能跑两者的翻译器是互斥的：qemu-user 只有 Linux ABI。

键只有 `e_machine` 时的实测后果（riscv64 宿主，已配 `a20.xlator.x86_64=/bin/qemu-x86_64`，喂一个 `e_machine=62` 且带 `PT_A20_START_INFO` 的合法 ELF）：

```
[XLATOR] pid=14 execve /tmp/xlate_native.elf (e_machine=62 abi=native) → /bin/qemu-x86_64
qemu-x86_64: /tmp/xlate_native.elf: Unable to find a guest_base to satisfy all guest address mapping requirements
```

qemu-user 没有崩，它退出 1。调用方拿到的是「guest 退出码 1」而不是 `ENOEXEC`——一次配置不匹配被报告成了 guest 的行为。`user/cmds/core/xlate_exec.c` 的 `native` 模式就是为了钉住这件事，`smoke-exec-xlator` 与 `smoke-exec-xlator-shim` 都断言它，且 shim 那条另外禁止 `[XLATOR] pid=` 行提到这个文件（`ENOEXEC` 本身不足以区分「键没命中」与「翻译器拒绝了它」）。

### ABI 为什么是**推导**出来的，不是配置的

`elf_probe_foreign()` 扫 program header 的那几行是整个设计里最需要解释的地方：`elf_load()` 早就在做同样的扫描（`PT_A20_START_INFO` → `elf_load_info_t.is_native_abi`），native 入口就是从 `ARG0` 读 `a20_start_info_t`（`kernel/proc/exec.c`）。把这个判定写在 loader 里，是因为它**本来就是**关于文件的事实——一个声称自己带 `PT_A20_START_INFO` 的镜像就是 native-ABI 程序，不管管理员配了什么。配置文件能说的话只有「谁接得住」，不是「它是什么」。

带来的直接好处是：将来加一个 native-ABI 翻译器是**注册表加一行**，不是 loader 加一条分支。完整的分步说明见 [04-extending.md](04-extending.md)。

扫不到 program header table 时按 Linux ABI 上报，而不是拒绝文件：那种头部本来就宣称了一张它没有的表，这是 `elf_check_header()` 那一侧的拒绝，调用方马上会走到那里；在 `elf_probe_foreign()` 里再判一次等于在不认识这个决定的地方重复它。

### 这个判定只有一处实现

`elf_phdrs_native()` 是「这张镜像说的是不是 A20 原生 ABI」这一个问题在内存里的**唯一**答案，三个 loader（`elf_load_from_buf()` 与 32/64 位那两条路径）都调它，`elf_probe_foreign()` 也调它。

这不是洁癖。这四类读者读的是**同一张 program header table**，而它们的结论必须一致：loader 说「这是原生 ABI」而转发钩子说「这是 Linux ABI」，得到的就是一个既不按 native 入口加载、也不转发的镜像——两份扫描里任何一份写错都是这种结果，而且两边都不会报错。之前每个 loader 各扫各的，加进来第四个读表的就成了第四份可能分叉的副本。

fd 那一侧（`elf_probe_foreign()`）先把整张表**一次读进内存**再交给同一个函数，而不是每个 program header 做一次 `lseek` + 56 字节 `read`：`MAX_PHDRS` 是 64，这条路径每次都会走满，而它跑在 exec 热路径上、且只对通道拒绝转发的外来镜像发生。

ABI 的**名字与反查**同理也只此一份：`xlator_abi_name()` / `xlator_abi_parse()` 定义在 `kernel/proc/xlator.c`、声明在 `kernel/include/proc/xlator.h`，启动日志、`/proc` 与转发那一行共用它们。曾经 `exec.c` 里另写了一份 `key.abi == ELF_ABI_NATIVE ? "native" : "linux"`，两份映射意味着加第三个 ABI 时只有一半的日志会知道它叫什么。

### 编译期名单仍然不存在

早期版本在 `mm/elf.c` 用 `switch` 写死 `EM_X86_64` / `EM_AARCH64`，`xlator.c` 里又用 `strcmp` 写了一遍，加上一处硬编码的计数，三处必须同步、漏一处就静默失效。现在 `elf_probe_foreign()` 只回答「这是一个格式合法的 ELF64，且 machine 不是本机，且它的 ABI 是什么」，「能不能翻译」交给 `xlator_lookup()`——也就是「管理员有没有为这个 (machine, ABI) 配过翻译器路径」。

「损坏文件绝不会被当成待翻译」这个安全性质没有因此变弱，反而论证更直接：magic/class/data/type/phentsize/phnum 全部由头部校验把关，走到这里说明头部合法；随后 `xlator_lookup()` 在配置表里按二元组查，`e_machine=0x9999` 这类值查不到任何条目，仍然走 `ENOEXEC`。删掉的是重复的第二份名单，不是这个性质。

**可翻译的集合是推导出来的，不是列出来的**：一个 (machine, ABI) 可翻译，当且仅当管理员为它配过路径。这正是「加一个架构或一种 ABI 不用改内核」的根据。

## 5. 接线点

`kernel/proc/exec.c` 的 `ENOEXEC → 回退循环`（`exec_try_script()` + `EXEC_RETRY` + 深度上限 `EXEC_MAX_DEPTH`）已经具备想要的形状：它能换一个镜像重来，且深度上限天然防住了递归。`exec_try_translator()` 与 `exec_try_script()` 平级，顺序在它之后：

```
elf_load 失败（-ENOEXEC）
  ├─ exec_try_script     → 是脚本则改写 argv，重来
  └─ exec_try_translator → 是外来架构且通道启用则改写镜像与 argv，重来
       └─ 都不适用 → -ENOEXEC（原样返回）
```

**shebang 优先于翻译是刻意的**：带 `#!` 的文件本来就是给内核解释的，不该被当成待翻译的 ELF。

`EXEC_RETRY` 复用现有深度上限：如果配置错误使翻译器自己也是外来架构，链路会在这里停住而不是无限递归。

## 6. 内核 API 契约

全部声明在 `kernel/include/proc/xlator.h`。`CONFIG_XLATOR=0` 时每个入口都有 stub（返回 `-ENOENT` / `-ENOSYS` / 无副作用），所以调用点不需要自己判断编译开关——这也是为什么 `kernel/fs/procfs/procfs.c` 能**无条件** include 这个头文件而不把通道拖进裁剪构建。

| 函数 | 契约 |
|---|---|
| `void xlator_config_init(void)` | 解析 `a20.xlator*` 键。必须在 `bootargs_init()` 之后调用一次；表此后只读 |
| `int xlator_enabled(void)` | 读一个原子量，可在 exec 热路径上从任意 CPU 调用 |
| `void xlator_set_enabled(int on)` | 运行期开关。影响**下一次** execve；已在跑的翻译器是普通进程，有意不打断 |
| `int xlator_lookup(const elf_guest_key_t *key, xlator_desc_t *out)` | 0 = 命中；`-ENOENT` = 通道关、或该 `(machine, ABI)` 无可用条目（调用方据此回到普通 `ENOEXEC` 路径）。按**整个二元组**匹配，见第 4 节 |
| `int xlator_parse_template(const char *tmpl, xlator_tmpl_t *out)` | 返回 token 数；`-EINVAL` = 未知 `@` 记号或无 `@P`；`-E2BIG` = 超长或 token 过多。启动时与 exec 时各调一次 |
| `int xlator_split_env(char *scratch, char **entries, int max)` | 就地校验并拆分 env 规格。**条目指向 `scratch`**，它必须活得比条目久；失败时拒绝整份规格而不是转发半懂的环境 |
| `void xlator_note_forward(void)` | 只在罕见的 re-exec 路径上调用，不是每次 exec |
| `int xlator_render(char *buf, size_t bufsz)` | `/proc/a20/xlator` 的渲染，返回写入字节数或负 errno |

### 三个结构体

`xlator_tmpl_t` 是一个**定长**对象：token 文本存在 `text[]` 的 `(start, len)` 窗口里，所以 exec 路径可以把它放在栈上，不需要第二次分配。

`xlator_desc_t` 里三个字符串都指向启动时写好的配置表（此后不再修改），所以借用是安全的——省掉了把 256 字节模板拷进调用者缓冲区。

`elf_guest_key_t` 是从 loader 借来的一个两字段结构，不是通道自己定义的类型——理由见第 4 节。它定义在 `kernel/include/mm/elf.h`，所以 `proc/xlator.h` include 了它；那条 include 不构成环（`mm/elf.h` 只拉 `core/types.h` 与 `mm/vm.h`）。

注册表的 `.def` 里 ABI 列写成 `XLATOR_ABI_LINUX` / `XLATOR_ABI_NATIVE` 两个宏而不是裸文本，因为 `.def` 按约定**没有自己的 `#include`**（与 `kernel/abi/<arch>/` 下的系统调用表一样），写不了 `ELF_ABI_LINUX`。两个宏定义在 `kernel/proc/xlator.c` 里唯一同时知道两种拼写的地方；写成裸的 `linux` / `native` 会让这一列成为没有任何工具交叉核对的拼写。

### 一次 `xlator_parse_template()`，两个调用点

启动时（配置校验）与 exec 时（展开）用的是同一个解析器，所以模板规则只有一处实现。这不是省事：两处实现必然漂移，而漂移的方向是「exec 路径展开出一个启动时认为非法的 argv」。

`CONFIG_XLATOR=0` 时常量与类型定义仍然留在 `#if` **之外**，因为 `#else` 的 stub 引用它们。这是一处曾经踩过的坑：把类型也关进 `#if` 里会让裁剪构建报 `unknown type name 'xlator_desc_t'`。

### 一个 guest 键怎么变成一个槽

`a20.xlator.x86_64.native.argv=…` 到 `g_slots[slot]` 之间有三步，每步只认一件事：

1. `xlator_field_of()` 先从**最后**一个点切出 `.argv` / `.env`，剩下的叫 selector。必须先切这两个：否则一个叫 `argv` 的 ABI 会赢走 `.argv` 的含义。
2. `xlator_selector_of()` 把 selector 拆成 `(名字, ABI)`。没有点就是裸名，等于 `ELF_ABI_LINUX`；有点则点后面必须是一个 `xlator_abi_parse()` 认识的 ABI。因为 guest 名字不能含点，这个切分没有歧义。
3. `xlator_slot_for()` 按**整个二元组**找行。

第 3 步按名字找曾经是对的，因为当时一个名字只可能有一行。现在同一个架构可以有两个 ABI 行，只比名字会返回**第一个**，于是第二行永远配不上——而且不会报错：每一条指名它的命令行都会被接受，然后存进错误的槽。加这一对比较的成本是一个字段比较，省掉它换来的是一个连日志都不会提的哑行。

配错时**必须区分是哪一半错了**，因为三种情况的排查方向完全不同：名字不认识（拼错了）、ABI 不认识（`linux`/`native` 之外，或者在 `.argv` 里被吃掉了一半）、名字认识但这一半没注册（一条看上去完全合理的配置，留给管理员的只有一个无从追查的 `ENOEXEC`）。这三种过去共用一句「未知外来架构」，而错得最多的那一种恰好把一个**认识**的架构报成不认识。

### 启动时解析的两处细节

**`a20.xlator` 是每个 guest 键的前缀。** 所以启用键必须由一个独立的 `xlator_parse_enable_key()` 处理，并带**精确**的 `=` 检查：否则 `a20.xlator.x86_64=/bin/qemu-x86_64` 会解析成启用键、值 `/bin/x86_64`，通道保持关闭而启动日志声称管理员要求了别的东西。那次失败除了一个费解的告警之外是静默的。

**规范化。** 校验通过后模板被重写回规范拼写（逗号分隔统一成空格），所以管理员写 `@P,@*` 与注册表默认值在 `/proc` 里显示同一个样子，两种来源可以直接对比。

## 7. 并发不变量

配置表在 `xlator_config_init()` 里写一次，之后只读，**因此不需要锁**——这个不变量写在 `kernel/proc/xlator.c` 的注释里，它成立的前提是没有任何运行期写入路径。唯一的运行期可变状态是 `g_enabled`，用 `__atomic_load_n/store_n`（ACQUIRE/RELEASE），照 `kernel/include/core/seqlock.h` 的先例。

`xlator_lookup()` 里**重新**断言了一次 `path_set && argv_valid && env_valid`，与 `config_init()` 结算时用的规则相同。重述一次是为了让 exec 路径不依赖「一定经过 `config_init()`」这个前提。

转发计数只在罕见路径上自增。

## 8. argv 改写的所有权

`exec_try_translator()` 复用 `exec_try_script()` 的所有权编排，但有一处不同：`@*` 展开时**移交**调用者 `argv[1..]` 的指针，而不是拷贝——guest 的长参数向量不该在一条本就罕见的路径上再付一次拷贝费。移交会把源槽置 `NULL`，于是 bprm 不再拥有它们，每根字符串恰好有一侧拥有。

失败路径（`ENOMEM` 或 `E2BIG`）释放 `new_args` 即可两侧都不漏：splice 已经把源置 `NULL`，被 splice 进来的那些在 `new_args` 里，其余在 `new_args` 里的是新拷贝。

新 argv 在**触碰 bprm 之前**先构建完成，所以中途失败时调用方的 bprm 仍然完整、`ENOEXEC` 路径仍然可回退。

env 注入发生在 argv 改写**安装之后**，所以它失败时 bprm 处于一致状态，调用方的 `bprm_free()` 能正常展开。

## 9. W^X：per-task 放行

`task_t.xlator_host` 仅由内核在 re-exec 路径上设置，`mm_wx_filter_prot()` 见此位即放行。它**不跨 fork 继承**：一个自身是外来架构的子进程必须自己再走一次 `execve` 与同一套检查。这与 fd/凭据类状态的语义不同，是有意的收紧。

放行路径打 `kinfo`，理由是可观测性优先于串口安静；代价是高 QPS 下日志偏吵。若日后需要降噪，应改为可配置级别，而不是删掉这行——它是「这个进程不受 W^X 保护」的唯一可见痕迹。

**放行是过度近似。** 内核给「被转发到的那个程序」开豁免，因为无从知道管理员指向的翻译器到底会不会 JIT——按需要 JIT 假设是唯一安全的方向，代价是一个**不** JIT 的翻译器也拿到了放行。要收紧就得在配置里加一条「这个翻译器需要 W^X 放行」的性质，而那等于让一个命令行字符串参与权限决定。所以现在是有意的多放行，不是疏漏。

## 10. 已知代价与未做的事

与 [01-usage.md](01-usage.md#9-已知边界) 的用户视角部分重复，这里补上「为什么」：

- **不做性能宣称。** 全部数据来自 QEMU TCG，翻译再叠一层仿真，性能数字不可信。验收只做功能性：能跑、输出对、argv 对、退出码对。
- **`wait4` 的一个次序疑点。** 用 CLOEXEC 错误管道做 exec 结果握手时，父进程可能在子进程 exec 完成前就进入 `waitpid`，A20OS 此时回 ECHILD。这是 harness 侧绕过的（去掉 `FD_CLOEXEC`，让写端随子进程退出才关闭），**没有**定位到内核根因，应作为后续观察项而不是已修的缺陷记录在案。
- **模板里的固定参数不能含空格或逗号。** 真要支持得给 cmdline 解析器加引号，那是一个通用解析器改动，不属于这个功能。
- **动态链接 guest、32 位 guest、翻译器的崩溃隔离与资源计量**，均未实现。
- **两个 guest 同时各跑一个自己的真翻译器没有测试**——仓里只有一种真翻译器。多个 guest 的并发**配置**已经跑通。
- **AOT / 编译器型翻译器没有实现**，需要的是新机制而不是新配置项，理由见 [02-integration.md](02-integration.md#5-aot-型翻译器prism-一类尚不支持)，接口上落在哪一步见 [04-extending.md](04-extending.md#5-编译器型aot翻译器需要的那一整块)。
- **没有预留 per-guest 的 vtable。** 现在剩下的那条轴（执行模型：包装器 vs 编译器）确实不是 argv 模板能表达的，但要给它建的不是一张全 NULL 的函数指针表，而是「同步等一个进程 + 产物缓存 + 产物的权限路径 + 新的错误契约」四块机制。理由与这一步什么时候该做，见 [04-extending.md](04-extending.md#4-为什么不预留一张-vtable)。
- **native-ABI 翻译器不存在**，所以 native-ABI 镜像一律 `ENOEXEC`。二元组键已经就位，加上它是一行注册表；为什么现在还没有，以及移植 qemu 的可行形态是什么，见 [04-extending.md](04-extending.md#3-加一个-native-abi-翻译器需要改什么)。
- **启动时不校验翻译器文件是否存在。**
- **一次 `smoke-exec-xlator` 超时未定位。** 首次运行该用例时超时（guest 到了 mksh 但没有命令执行），未改任何代码重跑即通过。没有复现，也没有定位。