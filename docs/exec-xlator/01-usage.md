# 使用指南：配置与排查

面向不熟悉 A20OS 的读者。读完这一篇你应该能自己配好通道、看清它在干什么、并在它不工作的时候定位到原因。**不需要读任何内核代码。**

## 1. 它在做什么

你 `execve` 一个外来架构的二进制时，内核按普通流程加载它，失败（`ENOEXEC`）。在这之后、内核准备放弃之前，它多问一个问题：

> 这个文件是一个**格式合法的**外来架构 ELF，它的 ABI 是哪一种，以及我为这个（架构, ABI）配过翻译器吗？

三个都是，才把这次 exec 改写成「执行那个翻译器，把这个外来镜像和它的参数交给它」。改写走的是 exec 自己的重试循环，所以对内核其余部分而言，这就是一次普通的 exec。

**为什么要问 ABI。** A20OS 有两种 ABI（Linux ABI 与 A20 native ABI），两种都是本机机器的 ELF64，唯一的区别是有没有 `PT_A20_START_INFO` 这个 program header。所以「架构」不足以说明「谁跑得了它」：qemu-user 只有 Linux ABI，遇到 native-ABI 镜像会失败。今天没有任何 native-ABI 翻译器，因此 native-ABI 镜像一律 `ENOEXEC`——见第 9 节。

结果：调用方的地址空间被翻译器接管，guest 在翻译器**自己的**地址空间里被翻译执行。

```
你的进程
  └─ execve("/bin/xlate_probe-x86_64", …)
       └─ 内核：不是本机 ELF → 读出 (e_machine, ABI) → 查配置
            ├─ 该 (e_machine, ABI) 有配置好的路径，且通道开启
            │    └─ 改写为 execve("/bin/qemu-x86_64", "-0", "<原 argv[0]>", "/bin/xlate_probe-x86_64", …)
            └─ 否则
                 └─ 返回 ENOEXEC（"Exec format error"），与未接入本特性时一致
```

## 2. 三个开关，各自独立

这三层互不依赖，能单独验证。把它们混成一层是这类功能常见的错误——一个 `a20.xlator=off` 既是「不编译」又是「不运行」，结果嵌入式构建为了省几 KB 仍然得把整段代码编进去。

| 层 | 开关 | 默认 | 关掉之后 |
|---|---|---|---|
| 编译期 | `CONFIG_XLATOR` | `y`（编进来） | 整块不参与构建：`kernel/proc/xlator.c` 不编译、`task_t` 不带该字段、`exec.c` 无此分支、符号表里搜不到 `xlator_*` |
| 启动时 | `a20.xlator=1` | 关 | `execve` 外来二进制返回 `ENOEXEC` |
| 运行期 | `/proc/a20/xlator` | 跟随启动键 | 下一次 `execve` 不再转发；已在跑的翻译器继续跑完 |

**默认编进来是安全的**：通道在运行期默认关闭，一个编进来但没配置的 `CONFIG_XLATOR=y` 内核，行为与没有这个特性的内核一致。

### 编译期什么时候会被强制关掉

三种情况，`Makefile` 会把 `CONFIG_XLATOR` 强制成 `n`（用户自己设的 `n` 永远不会被翻回 `y`）：

| 条件 | 原因 |
|---|---|
| `NOMMU=1` | 没有 MMU 可以按需调入 guest 内存 |
| `PROFILE=mcu` | 该 profile 本来就替换整个源文件列表 |
| `ARCH` 不在 `riscv64 loongarch64 aarch64 x86_64 ppc64le` 里 | 翻译器要 JIT guest 代码，需要 64 位 MMU 宿主和 `mprotect(RWX)`；32 位或 NOMMU 宿主做不到 |

验证裁剪真的生效（不依赖 QEMU）：

```
make ARCH=riscv64 CONFIG_XLATOR=0 dev-build
nm .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf | grep xlator   # 无输出
ls .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel/proc/xlator.o       # 不存在
```

`make smoke-exec-xlator-off` 用**完全相同的命令行**（仍然写着 `a20.xlator=1` 和翻译器路径）启动 `CONFIG_XLATOR=0` 的内核，断言外来探针是 `ENOEXEC`、日志里没有任何 `[XLATOR]` 行、`/proc/a20/xlator` 不存在。传了等于没传，才说明关掉的是编译而不是加了个运行时 no-op。

## 3. 启动配置

全部在**内核命令行**上。对齐 `a20.ufsd_blk` / `a20.tcpmode` 的风格。

```
a20.xlator=1                                  # 打开通道
a20.xlator.x86_64=/bin/qemu-x86_64            # 这个 guest 用哪个翻译器（必填，绝对路径）
a20.xlator.x86_64.argv=@P,@*                  # 可选：怎么调用它（省略则用注册表默认模板）
a20.xlator.x86_64.env=ROSETTA_TMPDIR=/tmp     # 可选：它需要什么环境（省略则不加）
```

### 完整键表

| 键 | 必填 | 取值 | 省略时 |
|---|---|---|---|
| `a20.xlator` | 是 | `1` 或 `0`，精确匹配 | 通道禁用，日志 `[XLATOR] 外来架构翻译通道: 禁用` |
| `a20.xlator.<guest>[.<abi>]` | 是（每个要转发的 guest） | 翻译器的**绝对路径**，必须以 `/` 开头 | 该 guest 按未配置处理，`execve` 返回 `ENOEXEC` |
| `a20.xlator.<guest>[.<abi>].argv` | 否 | argv 模板，见下节 | 用 `kernel/proc/xlator_guests.def` 里该 guest 的默认模板 |
| `a20.xlator.<guest>[.<abi>].env` | 否 | `NAME=VALUE,NAME=VALUE` | 不注入任何环境变量 |

`<guest>` 是**注册表里已有的名字**，`<abi>` 是 `linux` 或 `native`。**`<abi>` 可以省略，省略即 `linux`**——这是简写而不是通配：现存的每一条命令行都这么写，不该为了对称而改一遍；而通配语义意味着某天同一个架构注册了第二个 ABI 之后，所有旧配置会默默改指到另一行去。

```
a20.xlator.x86_64=/bin/qemu-x86_64            # 等价于 a20.xlator.x86_64.linux=…
a20.xlator.x86_64.native=/bin/prism-x86_64     # 同一架构的第二个 ABI
```

名字不能带 `.`（`.argv` / `.env` / ABI 后缀都是保留的），ABI 只有 `linux` 和 `native` 两个合法拼写。当前注册表里 `x86_64` 和 `aarch64` 都只有 linux 行，所以 `<abi>` 现在基本不写。

配置写错时启动日志会点名，而且**区分是哪一半错了**——这三种过去共用一句「未知外来架构」，而错得最多的那一种（`.native` 后缀还不被理解的时候）恰好把一个**认识**的架构报成不认识：

```
[XLATOR] 未知外来架构 'x86-64'，忽略（见 proc/xlator_guests.def）     # 名字不认识
[XLATOR] 未知 ABI 'natvie'，a20.xlator.x86_64.<abi> 忽略（当前可用 linux / native）  # ABI 拼错
[XLATOR] 'aarch64' 未注册 native ABI 的翻译器，忽略（见 proc/xlator_guests.def）   # 名字认识，这一半没有
```

第三种值得单说：架构认得、只是没为那个 ABI 注册行，是一条**看上去完全合理**的配置，留给管理员的只有一个无从追查的 `ENOEXEC`。
```

### 三条容易被违反的规则

1. **键的顺序不影响结果。** 内核读完整个命令行之后才统一结算，所以 `a20.xlator=1` 写在最后也一样生效。
2. **重复的键后者覆盖前者**，不追加——这样命令行保持单值，表的大小也有界。
3. **半份配置是不配置。** 模板或 env 规格在启动时解析失败，那个 guest 按「未配置」处理，`execve` 回到 `ENOEXEC`，**而不是**退回注册表默认模板继续转发。理由：管理员写下了一个路径和一个模板，其中一半没被理解——替他猜一个默认值继续跑，等于用一个他没要求过的 argv 去 exec 一个外部程序。

### 启动日志怎么读

配对了，每个可用的 guest 一行：

```
[XLATOR] x86_64 (e_machine=62 abi=linux) → /bin/qemu-x86_64  argv="-0 @A @P @*"
[XLATOR] x86_64 (e_machine=62 abi=linux) → /bin/xlate_shim  argv="@P @*" (cmdline 覆盖)  env="XLATOR_TEST_ENV=hello"
```

`(cmdline 覆盖)` 表示模板来自 `.argv` 键而不是注册表默认值。`abi=` 是查找键的一半，出现在这一行是为了让你能看出**是哪一半没命中**：镜像的架构配了路径但 ABI 不是 `linux`，和架构根本不认识，是两种不同的失败。配错了会逐个点名，不是一个笼统的失败：

| 日志 | 含义 |
|---|---|
| `[XLATOR] 已启用但未配置任何翻译器路径，execve 外来二进制仍将返回 -ENOEXEC` | 开了 `a20.xlator=1` 但一个 guest 路径都没给 |
| `[XLATOR] x86_64 (e_machine=62 abi=linux) 未配置翻译器，execve 将返回 -ENOEXEC` | 注册了这个 guest，但没给路径 |
| `[XLATOR] x86_64.linux argv 模板非法，execve 将返回 -ENOEXEC` | `.argv` 或默认模板没解析过 |
| `[XLATOR] x86_64.linux env 规格非法，execve 将返回 -ENOEXEC` | `.env` 没解析过 |
| `[XLATOR] a20.xlator.x86_64.linux 不是绝对路径，忽略` | 路径必须以 `/` 开头 |
| `[XLATOR] a20.xlator.x86_64.linux.argv='@Q,@P' 非法（未知 @ 记号或缺少 @P），该 guest 不会被转发` | 模板里有不认识的记号，或没有 `@P` |
| `[XLATOR] 未知 a20.xlator='on'，保持禁用` | 开关只接受 `0` / `1` |

每次真的转发时还有一行：

```
[XLATOR] pid=42 execve /tmp/guest_x86_64 (e_machine=62 abi=linux) → /bin/qemu-x86_64  argv="-0 @A @P @*"
```

**没有这一行**说明判定或接线根本没走到——先看 `[XLATOR]` 段有没有出现，再往下查。也可能是键只命中了一半：架构认出来了但 ABI 没命中（native-ABI 镜像就是这样），这时不会转发。

### 怎么把命令行传进去

各架构的来源不同，这是既有事实，不是本特性引入的差异：

| 架构 / 板子 | 内核从哪里拿命令行 | 在 QEMU 上怎么传 |
|---|---|---|
| riscv64 / ppc64le / riscv32 | FDT 的 `/chosen/bootargs` | QEMU `-append '…'` |
| aarch64（QEMU virt、VirtualBox） | QEMU `-kernel` 交进来的 DTB 的 `/chosen/bootargs` | QEMU `-append '…'` |
| x86_64（QEMU、x86_64-pc） | `-append` 经 fw_cfg，或 multiboot 命令行 | QEMU `-append '…'` |
| loongarch64 | 固件给的 DTB 的 `/chosen/bootargs`；DTB 里没有时，按需从串口收（`UART_CMDLINE=y`） | QEMU virt 见下 |
| arm32 / loongarch32 | 无（`arch_bootargs_get()` 显式返回 NULL） | 这些架构上本通道无法从命令行配置 |

**loongarch64 单独说一句。** 内核侧已经接上了：它从固件给的 DTB 里读 `/chosen/bootargs`，和别的架构走同一个 `fdt_extract_bootargs()`。但 QEMU 的 loongarch virt **根本不往内核送命令行**——实测过四条路都不通：全局 `-append`、`-machine virt,append=`、`-machine virt,dtb=<带 bootargs 的文件>`（QEMU 会重建 DTB 并丢掉它），以及 fw_cfg（这个二进制里根本没有 `qemu,cmdline` 项）。在运行中的 guest 里 dump 0x100000 那份 DTB，`bootargs` 这个字符串连影子都没有。

所以在没有别的输入源时，QEMU loongarch virt 上 `a20.*` 键全部无效，`/proc/a20/xlator` 会一直显示 `enabled: 0`。这不是内核侧还差什么，而是 QEMU 这条路没有提供输入——真实的 LS2K1000 UEFI 是否填这个属性**没有验证过**。

`make check-xlator-guests` 现在会强制「在 `XLATOR_SUPPORTED_ARCHES` 里的架构必须实现 `arch_bootargs_get()`」，就是为了不让这种「编进来了但永远配不上」的状态再无声地存在。

#### 没有固件时：从串口收命令行

```
make ARCH=loongarch64 ABI=both BRINGUP=0 XLATOR=1 UART_CMDLINE=y dev-build
```

`UART_CMDLINE=y` 让内核在**取不到**命令行时（上面那张表的第二列的所有来源都没给出东西）在串口上问一次：

```
[UARTCMD] type a kernel command line, or press Enter for none
[UARTCMD] a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64
[UARTCMD] using command line from the console
[FDT] bootargs='a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'
```

5 秒内没有输入（直接回车也算没有输入）就按「没有命令行」继续启动。整条串口路径只在 `arch_bootargs_get()` 的兜底里调用，`bootargs_init()` 本身不自作主张地问——`arch_bootargs_get()` 的结果会被缓存，问一次就够了。

**安全边界。** 命令行默认由**构建镜像的人**定；把它变成从串口读，等于把「内核配置——包括 `a20.xlator.<guest>`，也就是内核把一个外来镜像 re-exec 进哪个二进制」的权力交给**开机时拿着串口线的人**。所以这个开关默认关（`UART_CMDLINE ?= n`），关掉时零编译开销：整份 `kernel/core/uart_cmdline.c` 包在 `#ifdef CONFIG_UART_CMDLINE` 里，关掉后是一个空目标文件，编译行上不会出现该宏，启动日志里也不会有 `[UARTCMD]`。它不可能用「按键才触发」来保护自己——读命令行就是它存在的目的——所以只能在编译期关。`NOMMU=1` 与 `PROFILE=mcu` 下强制关掉：这两类构建没有可以停下来的串口交互。

**QEMU 上必须绕开 `-nographic`。** `-nographic` 等价于 `-serial mon:stdio`，输入先过 QEMU 的 mux，实测送不到 guest 的 16550（内核照常超时，只是读不到字节）。要手动加参数：

```
printf 'a20.xlator=1\n' | qemu-system-loongarch64 -machine virt -m 1G \
  -display none -monitor none -serial stdio …
```

并且**必须等提示出现之后再送**。QEMU 一拿到管道里的字节就交给仿真 UART，那比 guest 初始化 16550 早得多；早到的那部分会落进只有一字节的保持寄存器并被后面的字节覆盖掉。（内核侧因此把 FCR bit 0 打开了，让 16 字节 FIFO 接住一次按键的突发——但 FIFO 也救不了「在设备被编程之前就已经到达」的那一段。）

用 make 跑 QEMU 时，`EXTRA_QEMU` 会原样追加到 QEMU 命令行，是加 `-append` 的现成钩子：

```
make ARCH=riscv64 ABI=linux run \
  EXTRA_QEMU="-append 'a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'"
```

实例清单里对应 `machine.extra_qemu`，见 [../instances.md](../instances.md)。

**启动时不校验翻译器文件是否真的存在。** `xlator_config_init()` 跑在根文件系统挂载之前，无从校验；而挪到 exec 时校验等于把路径解析放回热路径。路径写错的表现是 exec 转发到翻译器时翻译器自己 `ENOEXEC`。

## 4. argv 模板语法

内核**不假设翻译器长什么样**。它唯一需要知道的是：给一个外来镜像，翻译器程序的 argv 应该是什么。

| 记号 | 展开成 |
|---|---|
| `@A` | 调用者的 `argv[0]`（没给就退回镜像路径） |
| `@P` | 外来镜像的路径 |
| `@*` | 调用者的 `argv[1..]`，原样插入 |

其余都是字面参数。分隔符是空格、制表或逗号。

| 想要的形状 | 模板 |
|---|---|
| qemu-user：选项在前，镜像单独一个位置参数 | `-0 @A @P @*` |
| Rosetta 风格：镜像本身就是 `argv[0]`，没有任何选项 | `@P,@*` |
| 选项带紧贴的值 | `--argv0=@A @P @*` |

**为什么 qemu 的默认模板需要 `@A`。** guest 常常按 `argv[0]` 分派，busybox 只有在 `argv[0]` 含 `busybox` 时才进多路调用模式，否则报 `applet not found`。用 `exec` 而不是 `exec -a` 语义上等价但容易被误读，所以直接给翻译器传 `-0`，由它去设 guest 的 `argv[0]`。

**`argv[0]` 永远是翻译器自己**，无论模板怎么写。内核 re-exec 的就是那个程序，模板若试图把镜像放到 `argv[0]`，那只是换个拐弯的说法说同一件事。

两条拒绝规则，都在启动时就地生效（未知记号几乎总是笔误，当字面量传下去等于给翻译器塞一个没人写过的参数；没有 `@P` 的模板无从说明要翻译哪个镜像）：

- 模板里有不认识的 `@` 记号 → 该 guest 不转发。
- 模板里没有 `@P` → 该 guest 不转发。

### 模板的已知限制

- **固定参数不能含空格或逗号。** 分隔符就是它们，所以 `--some-opt with space` 写不出来。guest 自己的参数不受影响（`@*` 原样插入）。
- **`@` 粘在字面量上会拆成两个 argv 槽。** `--argv0=@A` 展开成 `--argv0=` 和 `argv[0]` 两项。如果某个翻译器的选项真的要求字面量里含一个 `@A`，目前没有办法表达。
- 上限：模板 256 字节、最多 32 个 token（`kernel/include/proc/xlator.h` 里的 `XLATOR_TMPL_LEN` / `XLATOR_TMPL_MAX`）。

## 5. 环境变量注入

某些翻译器非有特定环境不可用——Rosetta 需要一个可写的 `ROSETTA_TMPDIR`。

```
a20.xlator.x86_64.env=ROSETTA_TMPDIR=/tmp/rosetta,LANG=C
```

规格是 `NAME=VALUE` 用逗号分隔。注入的条目**放在 envp 最前面**：查找取第一个匹配，所以前置才意味着管理员强制设的值真的压过调用者碰巧导出的同名变量。

限制：一条里不能含空格或逗号；最多 8 条、每条 64 字节（`XLATOR_ENV_MAX` / `XLATOR_ENV_ENTRY_MAX`）。注入的字节数计入和调用者自己的 argv/envp 同一份栈预算，所以配一个很大的环境会让一次本来成功的 exec 变成 `E2BIG`。

## 6. 运行期开关

`/proc/a20/xlator` 可读可写，模式 0644。

读：

```
$ cat /proc/a20/xlator
enabled: 1
forwards: 3
registered: 2
guest x86_64: machine=62 abi=linux path=/bin/qemu-x86_64 argv="-0 @A @P @*" env=(none)
guest aarch64: machine=183 abi=linux path=/bin/xlate_shim argv="@P @*" (override) env=XLATOR_TEST_ENV=hello
```

- `enabled`：当前开关。`forwards`：成功转发的累计次数。`registered`：注册表里的 guest 总数。
- 每个 guest 一行给出 `machine`（`e_machine` 数值）、`abi`、`path`、`argv`、`env`。**`path=(none)` `argv="(unset)"` 表示这个 guest 不可用**——是没配路径，还是模板/env 非法，这里看不出来，回启动日志找具名的那一行。
- **`abi` 是查找键的一半，而不只是说明文字。** 同一台机器的两种 ABI 在 `machine` 上长得一样，转发哪个由它决定。一个 native-ABI 镜像会落到没有 `abi=native` 行的那个结果上，也就是 `ENOEXEC`——这不是「翻译器没配好」，是这条通道今天不服务这种 ABI。
- `(override)` 表示模板来自 `.argv` 键。
- 模板与 env 都规范化成一种拼写：管理员写 `@P,@*`，这里显示 `@P @*`。这样「命令行覆盖」和「注册表默认」两种来源可以直接对比。

写 `0` 或 `1` 切换，其余输入 `-EINVAL`：

```
echo 1 > /proc/a20/xlator     # 打开
echo 0 > /proc/a20/xlator     # 应急关闭
```

这里用精确比较而不是 `atoi`，因为 `atoi` 把 `"1x"`、`"2"`、`""` 全都变成 0——一个被静默接受的笔误会让人以为「关掉了」，而那恰好是急着关通道的人最需要得到真相的时刻。写别的值返回 `EINVAL` 且开关不变。

两条边界要清楚：

- **这个节点只管开关，不改路径。** 换翻译器要重启。写这个节点不新增任何权限：能 exec 那个翻译器路径的进程本来就能直接 exec 它，通道只是省掉拼命令行这一步；真正决定权限的是启动时写了什么路径。
- **关掉时已在运行的翻译器进程继续跑完。** 它此刻就是一个普通进程。有意如此：应急闸门不该变成杀进程。

## 7. 不用交叉编译器的验证路径

想确认「内核拼出来的 argv 到底长什么样」，不需要真的下载 qemu，也不需要交叉编译器。仓里有一个只打印自己 argv 和 env 的宿主程序 `/bin/xlate_shim`（`user/cmds/core/xlate_shim.c`），它什么都不翻译。

`/bin/xlate_exec`（`user/cmds/core/xlate_exec.c`）是 Linux-ABI 启动器，两个模式配合它用：

```
/bin/xlate_exec stage 62 /tmp/guest_x86_64      # 造一个 e_machine=62 的合法 ELF64 头
/bin/xlate_exec stage 183 /tmp/guest_aarch64   # 同上，e_machine=183
/bin/xlate_exec run /tmp/guest_x86_64 ALPHA BETA
```

`stage` 造的是**没有** `PT_A20_START_INFO` 的 Linux-ABI 镜像。对照组是另一个模式：

```
/bin/xlate_exec native ignored                 # 期望：XLATE_EXEC: native PASS: ENOEXEC
```

它造一个 `e_machine=62` **带** `PT_A20_START_INFO` 的镜像——也就是同架构、native ABI。当 x86_64 配了翻译器时它仍然必须是 `ENOEXEC`，因为没有 `abi=native` 的注册行。这条断言挡的是一个静默故障：键只按架构匹配时，这个文件会被交给 qemu-x86_64，调用方拿到的是「guest 退出码 1」而不是 `ENOEXEC`。

用 `@P,@*` 模板时，shim 会看到：

```
XLATE_SHIM: argv[0]=/bin/xlate_shim
XLATE_SHIM: argv[1]=/tmp/guest_x86_64      <- 镜像路径就是第一个位置参数，前面没有多余的 argv[0]
XLATE_SHIM: argv[2]=ALPHA
XLATE_SHIM: argv[3]=BETA
```

用默认 `-0 @A @P @*` 模板时：

```
XLATE_SHIM: argv[1]=-0
XLATE_SHIM: argv[2]=decoy-argv0             <- 调用者的 argv[0]，用一个诱饵证明它不是路径
XLATE_SHIM: argv[3]=/tmp/guest_aarch64
```

`make smoke-exec-xlator-shim` 一次把两种形状都断言掉。

## 8. 排查

| 现象 | 先看 | 常见原因 |
|---|---|---|
| `Exec format error` | 有没有 `[XLATOR] pid=` 行 | 没有这一行 → 通道关着、没配路径、或这个 `(e_machine, abi)` 不在注册表里 |
| `Exec format error`，但它明明是 A20OS 编译的 `.a20drv` / 外来架构 native 程序 | 那一行有没有 `abi=native` | 通道只服务 Linux ABI。native-ABI 镜像要走 A20OS 自己的加载路径，不是这条路 |
| guest 退出码是 1 而不是 `Exec format error` | 翻译器自己的输出 | 这个镜像可能不是 Linux-ABI 的 guest（qemu-user 会抱怨 `Unable to find a guest_base…`）。用 `/bin/xlate_exec native` 确认通道对不对 |
| `Exec format error`，日志有 `[WARN]` | 启动日志的具名告警 | 路径不是绝对路径 / 模板非法 / env 非法 / guest 名字或 ABI 拼错 |
| `Exec format error`，日志干净 | `cat /proc/a20/xlator` | 该 guest 行显示 `path=(none)` 或 `argv="(unset)"` |
| `/proc/a20/xlator` 不存在 | 构建配置 | 这是一个 `CONFIG_XLATOR=0` 的内核 |
| 转发成功但 guest 行为不对 | shim 打出的 `argv[i]=` | 模板形状与翻译器不匹配，见第 4 节 |
| 翻译器起来了但 guest 在 syscall 面上崩 | guest 自己的输出 | 与本特性无关：guest 需要的 syscall 覆盖见 `kernel/abi/linux/syscall_coverage.md` |
| 翻译器报 JIT 相关的 `mprotect` 失败 | `W^X 策略` 日志行 | 见下一节 |

失败时的日志落点：`.kernel-build/smoke/exec-xlator-riscv64.log`（`smoke-exec-xlator`）与 `.kernel-build/smoke/exec-xlator-shim-riscv64.log`（shim 用例）。

### W^X：per-task 放行，不是全局关策略

翻译器必须把 guest 代码 JIT 进 RWX 缓冲区，A20OS 的用户态 W^X 门（`kernel/mm/wx.c`）会拦下它。

**这棵树的用户态 W^X 默认策略是 `off`**，由 `a20.wx=deny|strip|off` 决定（`kernel/mm/wx.c` 里的默认值是刻意的：V8 之类会把代码页 mprotect 成 W|X 再写，严格策略不会让内核更难破，只会让它起不来）。

本实现**没有**让这条路径靠 `a20.wx=off` 放行——那等于为一个功能关掉全系统的策略。做法是 `task_t.xlator_host`：仅由内核在 re-exec 路径上设置，`mm_wx_filter_prot()` 见此位即放行。所以如果你看到 `W^X 策略: off`，说明有人用全局开关换取了翻译器能跑，这不是本实现认可的配置。

要在自己的板子上验证这条放行路径确实有效，命令行要**显式**加上 `a20.wx=deny`：`smoke-exec-xlator` 与 `smoke-exec-xlator-la64` 都这么做，并且禁止 `W^X 策略: off` 出现在日志里。默认策略是 off 的时候放行路径照样会触发（豁免是按任务判的，与策略无关），但那时它什么也没证明——没有东西被拒绝过。

放行路径会打 `kinfo` 日志，代价是高 QPS 下日志偏吵；它是「这个进程不受 W^X 保护」的唯一可见痕迹，所以不要删。若日后需要降噪，应改为可配置级别。

## 9. 已知边界

这些是**有意如此或尚未实现**，不是待办事项：

- **启动时不校验翻译器文件存在**，理由见第 3 节。
- **路径只在启动时解析**，运行期不能改。
- **W^X 放行是过度近似。** 内核给「被转发到的那个程序」开豁免，因为无从知道管理员指向的翻译器到底会不会 JIT——按需要 JIT 假设是唯一安全的方向，代价是一个**不** JIT 的翻译器也拿到了放行。要收紧就得在配置里加一条「这个翻译器需要 W^X 放行」的性质，而那等于让一个命令行字符串参与权限决定。
- **固定模板参数与 env 条目不能含空格或逗号**，见第 4、5 节。
- **只服务 Linux ABI 的 guest。** 通道的键是 `(e_machine, abi)`，而注册表里只有 `abi=linux` 的行——因为 qemu-user 只有 Linux ABI。所以一个带 `PT_A20_START_INFO` 的外来架构镜像（native-ABI）会得到 `ENOEXEC`，而不是被送进一个装不下它的翻译器。要加 native-ABI 的翻译器是**加一行注册表**，但它本身还不存在；见 [04-extending.md](04-extending.md#3-加一个-native-abi-翻译器需要改什么)。
- **动态链接 guest、32 位 guest、翻译器的崩溃隔离与资源计量**，均未实现。通道目前只承载「一个外来静态 Linux-ABI 可执行文件跑完并返回退出码」这一件事。
- **多个 guest 的并发配置**已经跑通（`smoke-exec-xlator-shim` 同时配了 `x86_64` 和 `aarch64`），但**两个 guest 同时各跑一个自己的真翻译器**没有测试——仓里只有一种真翻译器。
- **逐架构的运行期开关**没有做，只做了通道级。理由：那是在一个已经够大的控制面上再加一层，而「已配置 / 未配置」已经表达了等价的可调度状态。
- **没有任何性能宣称。**

下一步：想接入自己的翻译器，读 [02-integration.md](02-integration.md)；想扩展这条通道本身（新增 ABI、AOT 型翻译器、Linux binfmt_misc 的借鉴与教训），读 [04-extending.md](04-extending.md)。