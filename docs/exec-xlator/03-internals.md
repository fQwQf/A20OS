# 实现与设计取舍

面向要改这段内核代码的人。**配置怎么用见 [01-usage.md](01-usage.md)，怎么接入翻译器见 [02-integration.md](02-integration.md)**，这里只讲「为什么是这个形态」「代码在哪」「契约与不变量是什么」。

## 1. 代码地图

| 文件 | 角色 |
|---|---|
| `kernel/include/proc/xlator.h` | 全部 API 与契约。`CONFIG_XLATOR=0` 时提供 stub，调用点因此不必自己加 `#ifdef` |
| `kernel/proc/xlator.c` | 启动时解析 `a20.xlator*` 键；运行期开关与 `/proc` 渲染。整个文件体包在 `#ifdef CONFIG_XLATOR` 内，**不写 `#else`** |
| `kernel/proc/xlator_guests.def` | 唯一的 guest 注册表（X-macro，被 `xlator.c` `#include`，同时被 `tools/` 解析） |
| `kernel/proc/exec.c` | `exec_try_translator()`：argv 改写 + env 注入 + W^X 标记 |
| `kernel/mm/elf.c` | `elf_is_foreign_arch()`：只回答「这是一个格式合法的外来架构 ELF64」 |
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
XLATOR_GUEST(x86_64,  62,  "-0 @A @P @*")
XLATOR_GUEST(aarch64, 183, "-0 @A @P @*")
```

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

## 4. 外来架构判定：与「坏格式」严格分开

外来 machine 与文件损坏原本共用同一个 `-ENOEXEC`：`kernel/mm/elf.c` 的判定返回 bool，两种情形在 `elf_load` 里坍缩成同一个错误码。直接复用它无法区分「请翻译这个」和「这个文件坏了」，误判会让坏文件被送进翻译器。

因此新增 `elf_is_foreign_arch(fd, &machine)`，**只在 `proc_exec` 的 ENOEXEC 分支按需调用**，不改动 `elf_load` 的返回值契约。它读头部并校验 magic/class/data/type/phentsize/phnum，然后：

- 宿主原生 machine → 返回 `-ENOEXEC`（走原路径，不算外来）；
- 其它 machine → 返回 0 并输出 machine，**是否真的可翻译由通道配置回答**。

这里刻意不再有一份编译期名单。早期版本在这里用 `switch` 写死 `EM_X86_64` / `EM_AARCH64`，`xlator.c` 里又用 `strcmp` 写了一遍，加上一处硬编码的计数，三处必须同步、漏一处就静默失效。现在 `elf_is_foreign_arch()` 只回答「这是一个格式合法的 ELF64，且 machine 不是本机」，「能不能翻译」交给 `xlator_lookup()`——也就是「管理员有没有为这个 machine 配过翻译器路径」。

「损坏文件绝不会被当成待翻译」这个安全性质没有因此变弱，反而论证更直接：magic/class/data/type/phentsize/phnum 全部由头部校验把关，走到这里说明头部合法；随后 `xlator_lookup()` 在配置表里按 machine 查，`e_machine=0x9999` 这类值查不到任何条目，仍然走 `ENOEXEC`。删掉的是重复的第二份名单，不是这个性质。

**可翻译的架构集合是推导出来的，不是列出来的**：一个 machine 可翻译，当且仅当管理员为它配过路径。这正是「加一个架构不用改内核」的根据。

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
| `int xlator_lookup(uint16_t machine, xlator_desc_t *out)` | 0 = 命中；`-ENOENT` = 通道关、或该 machine 无可用条目（调用方据此回到普通 `ENOEXEC` 路径） |
| `int xlator_parse_template(const char *tmpl, xlator_tmpl_t *out)` | 返回 token 数；`-EINVAL` = 未知 `@` 记号或无 `@P`；`-E2BIG` = 超长或 token 过多。启动时与 exec 时各调一次 |
| `int xlator_split_env(char *scratch, char **entries, int max)` | 就地校验并拆分 env 规格。**条目指向 `scratch`**，它必须活得比条目久；失败时拒绝整份规格而不是转发半懂的环境 |
| `void xlator_note_forward(void)` | 只在罕见的 re-exec 路径上调用，不是每次 exec |
| `int xlator_render(char *buf, size_t bufsz)` | `/proc/a20/xlator` 的渲染，返回写入字节数或负 errno |

### 三个结构体

`xlator_tmpl_t` 是一个**定长**对象：token 文本存在 `text[]` 的 `(start, len)` 窗口里，所以 exec 路径可以把它放在栈上，不需要第二次分配。

`xlator_desc_t` 里三个字符串都指向启动时写好的配置表（此后不再修改），所以借用是安全的——省掉了把 256 字节模板拷进调用者缓冲区。

### 一次 `xlator_parse_template()`，两个调用点

启动时（配置校验）与 exec 时（展开）用的是同一个解析器，所以模板规则只有一处实现。这不是省事：两处实现必然漂移，而漂移的方向是「exec 路径展开出一个启动时认为非法的 argv」。

`CONFIG_XLATOR=0` 时常量与类型定义仍然留在 `#if` **之外**，因为 `#else` 的 stub 引用它们。这是一处曾经踩过的坑：把类型也关进 `#if` 里会让裁剪构建报 `unknown type name 'xlator_desc_t'`。

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
- **AOT / 编译器型翻译器没有实现**，需要的是新机制而不是新配置项，理由见 [02-integration.md](02-integration.md#5-aot-型翻译器prism-一类尚不支持)。
- **启动时不校验翻译器文件是否存在。**
- **一次 `smoke-exec-xlator` 超时未定位。** 首次运行该用例时超时（guest 到了 mksh 但没有命令执行），未改任何代码重跑即通过。没有复现，也没有定位。