# 接入指南：把你的翻译器挂进这条通道

面向要提供外来架构执行能力的开发者。读完你应该能判断自己的翻译器属于哪一类、需要满足什么契约、要改几处、怎么验证。

**配置项的完整定义在 [01-usage.md](01-usage.md)，这里不复述**，只讲接入决策。

## 1. 先分类：你的翻译器是哪一类

这个判断决定了一切。通道的机制是「exec 时改写 argv 再 exec 一次」，它只能承载**包装器**。

| | 包装器型（qemu-user、Rosetta 2 这类） | 编译器型（Prism 这类） |
|---|---|---|
| 输入 | 外来镜像路径 | 一个模块 |
| 做什么 | 在自己地址空间里翻译并执行 | 跑一遍，**产出**一段本地产物 |
| 内核要做的 | 把镜像路径和参数交给它 | 同步等它跑完，再 exec 它的产物 |
| 支持情况 | **已支持，零代码改动** | **尚不支持**，见第 5 节 |

判据很简单：**你的程序能不能吃下一个镜像路径、然后自己把它跑完并返回它的退出码？** 能，就是包装器型，落到第 3 节。不能（需要先产出别的东西），就是编译器型，落到第 5 节。

### 先确认一件更早的事：你的翻译器跑哪种 ABI

A20OS 有两种 ABI（Linux ABI 与 A20 native ABI），通道的键是 `(架构, ABI)` 二元组。**qemu-user 只有 Linux ABI**——它实现的是 Linux 的 syscall ABI，遇到带 `PT_A20_START_INFO` 的镜像会直接失败。

所以先问：你要服务的是 Linux-ABI 的 guest，还是 native-ABI 的 guest？

- **Linux-ABI** → 下面第 3 节，适用于今天，零代码改动。
- **native-ABI** → 这条路今天走不通，因为通道里没有任何 native-ABI 翻译器。加一个需要哪些改动、为什么移植 qemu 不是配置项，见 [04-extending.md](04-extending.md#3-加一个-native-abi-翻译器需要改什么)。

想亲眼看到通道怎么拒绝 native-ABI 镜像：

```
/bin/xlate_exec native ignored      # 期望：XLATE_EXEC: native PASS: ENOEXEC
```

## 2. 包装器的契约

内核对翻译器程序的要求只有四条：

1. **宿主原生可执行文件。** 必须是**构建宿主**那个架构的 ELF。这不是建议：宿主版的翻译器若被做成外来架构，它自己会撞上本特性然后被转发给它自己。`tools/xlator_fetch.py` 的 `--check-guest` 就是断言这一条。
2. **宿主 Linux ABI 下可运行。** 它对 A20OS 而言就是一个普通 Linux-ABI 程序，走 musl 生态那一套。
3. **吃下镜像路径。** 路径会作为某个位置参数交给它，位置由你的模板决定。
4. **返回 guest 的退出码。** 内核不代理任何东西，退出码就是翻译器的退出码。

内核**不做**的事（所以这些是你的责任）：不检查文件是否存在（启动时校验不了，见 [01-usage.md](01-usage.md#启动日志怎么读)）、不做崩溃隔离、不做资源计量、不解析你的输出。

## 3. 接入一个包装器：零处代码改动

`.def` 不动，内核代码不动，`tools/` 不动。Rosetta 与 qemu-user 的真实差别只有两处——argv 形状和环境——而这两处恰好就是 `.argv` 和 `.env` 两个启动键。

```
a20.xlator=1
a20.xlator.x86_64=/usr/libexec/rttranslator
a20.xlator.x86_64.argv=@P,@*
a20.xlator.x86_64.env=ROSETTA_TMPDIR=/tmp/rosetta
```

### 第 1 步：选模板

模板是你的翻译器的调用约定，**不是 guest 架构的属性**。同一个 guest 在不同板子上可能由 qemu-user 服务，也可能由仓内自研的翻译器服务。所以它是启动键（有注册表默认值），而不是 guest 表里的一列。

| 你的翻译器长什么样 | 模板 | 内核拼出的 argv |
|---|---|---|
| `rttranslator <path> <args...>` | `@P,@*` | `[rttranslator, <path>, <args...>]` |
| `qemu-x86_64 -0 <argv0> <path> <args...>` | `-0 @A @P @*` | `[qemu-x86_64, -0, <argv0>, <path>, <args...>]` |
| `mytrans --argv0=<v> <path> <args...>` | `--argv0=@A @P @*` | `[mytrans, --argv0=<argv0>, <path>, <args...>]` |

注意 `argv[0]` 恒为翻译器自身，内核在 `[0]` 位置放的就是它，模板从 `[1]` 开始展开。所以模板里**不要**再放一次翻译器名字。

`@A` 展开成调用者的 `argv[0]`，也就是调用方 `execve` 时给的名字，不是路径。想让 guest 按自己的程序名分派（busybox 那种）就需要它；想让路径直接当 `argv[0]` 就用 `@P` 打头、不放 `@A`。

### 第 2 步：写清环境需求

如果你的翻译器非有特定环境不可用，列在 `.env` 里。这些值会**前置**到 envp，压过调用者碰巧导出的同名变量：

```
a20.xlator.x86_64.env=ROSETTA_TMPDIR=/tmp/rosetta
```

多条用逗号分隔。放不进含空格或逗号的单条，最多 8 条 × 64 字节。

### 第 3 步：用 shim 验证形状，**不要**用真翻译器验证

这是本指南最重要的一条实践。

一个真翻译器会**容忍**错误的 argv——qemu 拿到乱序的参数也有自己的猜测逻辑。所以拿 qemu 做断言，测出来的是「qemu 忍住了」，不是「内核拼对了」。这类假通过正是本特性早期把 `argv0_flag` 换成模板的动因：`-0` 写错了，qemu 照样把 busybox 跑起来。

正确做法是指向仓内的 shim——`user/cmds/core/xlate_shim.c`，一个**什么都不翻译、只打印自己 argv 和 env** 的宿主原生程序。它不容忍任何东西，所以错的 argv 会直接显示成错的 argv。

```
a20.xlator=1
a20.xlator.x86_64=/bin/xlate_shim  a20.xlator.x86_64.argv=@P,@*
a20.xlator.aarch64=/bin/xlate_shim
```

配好后启动，内核日志与 shim 输出：

```
[XLATOR] x86_64 (e_machine=62 abi=linux) → /bin/xlate_shim  argv="@P @*" (cmdline 覆盖)
[XLATOR] aarch64 (e_machine=183 abi=linux) → /bin/xlate_shim  argv="-0 @A @P @*"
...
XLATE_SHIM: argv[0]=/bin/xlate_shim
XLATE_SHIM: argv[1]=/tmp/guest_x86_64      <- Rosetta 形状：路径是第一个位置参数，无选项
XLATE_SHIM: argv[2]=ALPHA
XLATE_SHIM: argv[3]=BETA
XLATE_SHIM: env XLATOR_TEST_ENV=hello
...
XLATE_SHIM: argv[1]=-0                      <- 注册表默认形状
XLATE_SHIM: argv[2]=decoy-argv0             <- @A 生效，且它不是路径
XLATE_SHIM: argv[3]=/tmp/guest_aarch64
```

`make smoke-exec-xlator-shim` 跑的就是这件事：**两个形状不同、约定不同的 guest 同时配好、各走各的形状**。这是「换一个翻译器不用改内核」的直接证据，而且不下载任何东西、不需要交叉编译器。

对应地，把 shim 换成你真正的翻译器之前，先确认上面这些行在你的配置下也成立——尤其 `argv[1]` 是路径而不是一个残留的调用者 `argv[0]`。

### 第 4 步：确认退出码与信号

通道不代理退出码，翻译器的退出码就是 guest 的退出码。`smoke-exec-xlator` 断言了这一点：guest 的标记串、`argv`、退出码三者都正确。

## 4. 加一个新的 guest 架构

加**架构**和加**翻译器**是两件不同的事，成本也不同：

- 加一个**翻译器** = 一次启动配置改动，零代码（本文第 3 节）。
- 加一个 **guest 架构** = 两处编辑，外加过一道门禁。
- 加一个 **guest ABI**（同一架构的第二种 ABI，例如将来的 native-ABI 翻译器）= `.def` 加一行，外加门禁的一条新断言。CC 表那条「两处编辑」对它不直接适用，理由与处理方式见 [04-extending.md](04-extending.md#第-3-步tools-targets-xlator-mk-加交叉编译器)。

两处编辑：

**1. `kernel/proc/xlator_guests.def`** —— 唯一的 guest 注册表：

```
XLATOR_GUEST(<name>, <e_machine>, <abi>, <argv_template>)
```

| 列 | 要求 |
|---|---|
| `<name>` | 合法标识符片段；**不能含 `.`**（`.argv` / `.env` 是保留后缀）；会同时用作 shell 变量后缀和 C 字符串 |
| `<e_machine>` | 十进制数（这个文件不含内核 include，写不了 `EM_X86_64`）；必须与 `kernel/include/mm/elf.h` 里的 `EM_*` 一致 |
| `<abi>` | `XLATOR_ABI_LINUX` 或 `XLATOR_ABI_NATIVE`，必须对应 `elf.h` 里真实存在的 `ELF_ABI_*`。写成裸的 `linux` / `native` 会被门禁拒掉：`.def` 按约定没有自己的 `#include`，用宏名是让「这一列对应内核哪个枚举」可被工具交叉核对的唯一办法 |
| `<argv_template>` | 带引号的字符串字面量，模板语法见 [01-usage.md](01-usage.md#4-argv-模板语法)；必须含 `@P`，只能用 `@A`/`@P`/`@*` |

ABI 列不是元数据，它是**查找键的一半**：同一台机器的两种 ABI 在 `e_machine` 上无法区分，而能跑它们的翻译器是互斥的。键只有 `e_machine` 时会发生什么，是实测出来的而不是推出来的，见 [03-internals.md](03-internals.md#为什么输出的是二元组)。

**2. `tools/targets-xlator.mk`** —— 一行 `XLATOR_GUEST_CC_<name> := <交叉编译器>`。

这一行**无法从 `.def` 推导**——它是构建环境事实（宿主上装了哪个 gcc）。所以只能断言一致，不能计算。`make check-xlator-guests` 负责这个断言：

- `.def` 里每个 guest 都有对应的 `XLATOR_GUEST_CC_<name>`；
- 反向也成立（多余的 CC 定义就是漂移）；
- `.def` 的 `e_machine` 与 `elf.h` 的 `EM_*` 一致；
- `.def` 的 `abi` 对应 `elf.h` 里真实存在的 `ELF_ABI_*`；
- 每个模板只含内核认识的记号，且出现 `@P`。

倒数第二条对应第 4 节那个实测出来的误转发，所以它不是「多查一个字段」那么轻：这一列写错，内核**编译得出来、启动日志也干净**，只是那一行永远匹配不到，每次 exec 都 `ENOEXEC`，而启动日志只会说这个 guest「未配置翻译器」。

最后一条是对内核 `xlator_parse_template()` 的**复述**，不是第二个实现——内核才是权威，并且在启动时就地拦住。门禁的价值只是让同样的错误在一秒内在宿主上暴露，而不是在目标机的启动日志里。

这道门禁不是预防性的摆设：写下它的时候两份清单**已经漂移**了——`XLATOR_GUEST_CC_riscv64` 存在，而 `--guest riscv64` 会被 argparse 直接拒掉。删掉那行没有损失任何能力（那个 guest 从来就不能翻译），但它说明了为什么需要门禁。

`tools/xlator_fetch.py` 原本自带一份 `GUEST_TARGETS` 字典，那正是「第三份名单」的来源。它现在直接解析 `.def`，`--guest` 的可选值由内核的注册表决定。

新增或修改 guest 时的正确顺序：**先改 `.def`，再改 `targets-xlator.mk`，然后 `make check-xlator-guests`。** 反过来做，门禁会告诉你缺哪一半。

### 要不要下载翻译器

仓里只自带 qemu-user 的取件逻辑（`tools/xlator_fetch.py`，从 Alpine v3.23 的 APKINDEX 解析版本化 `.apk`、解开拼接的 gzip 载荷、取出翻译器并校验它是宿主原生 ELF）。你自己的翻译器不进 git，按普通方式放进 rootfs 即可，然后按路径配置。

要跑完整的真实翻译验证（而不是 shim），构建时带上翻译器与探针：

```
make ARCH=riscv64 ABI=linux XLATOR=1 XLATOR_GUEST=x86_64 dev-build
make smoke-exec-xlator
```

`XLATOR=1` 让这两个产物成为镜像的**前置依赖**（`$(FAT32_IMG)` 依赖它们），而不是一个要你记得先跑的 `make xlator-assets`。这不是洁癖：产物落在 `$(USER_BUILD_DIR)`，而 `make -C user clean` 会在用户态 build id 变化时清空该目录，写成 `pre` 步骤就会被删掉，表现为「镜像里少一个二进制」，看起来像内核问题。

### 探针不是装饰，是断言的载体

`user/cmds/core/xlate_probe.c` 按需交叉编译成 `xlate_probe-<guest>`，宿主原生那份作对照组。构建后有一条 `--check-guest` 断言逐个核对 `e_machine`：**翻译器必须宿主原生，探针必须是 guest 架构**。

这条断言针对的是一个已经真实发生过的错误——变量曾按 `$(ARCH)`（宿主）而非 `$(XLATOR_GUEST)` 取值，探针被编译成了 riscv64，于是 smoke 安静地跑成了一个原生回归，翻译器从未被触及。没有这条断言时，那类错误不会让门禁变红，只会让它变得没有意义：绿色的门禁，测的是完全另一件事。

推广一下：**验证「翻译发生了」的用例，其探针必须确实是外来架构**，否则用例可以在从未触及翻译路径的情况下通过。这和第 3 节那条「不要用真翻译器验证 argv」是同一个陷阱的两面。

## 5. AOT 型翻译器（Prism 一类）：尚不支持

Prism 不是包装器，是**提前编译器**：输入一个模块，输出一段本地产物，然后执行那个产物。

这类翻译器要接进来，缺的不是配置项，而是一整段机制：

- **谁来跑编译器。** 当前模型是「exec 时重写 bprm 再 exec 一次」，内核从不等待另一个程序的产出。要支持 AOT，就得在 exec 的热路径上同步地跑一个进程、读它的产出、再去 exec 产物。
- **产物放哪。** 需要一个内核管理的缓存目录（名字要由镜像路径 + 内容派生，否则要么每次重编要么撞名），需要淘汰策略，需要配额。
- **产物是什么 ELF。** 它是宿主原生镜像，所以 `elf_load` 能直接吃——这一点反而是顺的。但它必须可写、可执行，而 `CONFIG_XLATOR` 目前给转发出去的进程开的是 W^X 放行，不是文件权限。
- **失败怎么办。** 编译器崩溃、缓存写满、目标不被支持——每一种都要一个新的错误路径，而不是复用 `ENOEXEC`（`ENOEXEC` 已经明确表示「这不是能跑的东西」，拿来表示「翻译器编译失败」是撒谎）。

这一整块是**独立的功能**，不是本机制的一个配置项。所以这里不写半成品：既没有为它预留一个假的 `aot` 枚举值，也没有暗示「配一下就能用」，也没有为此预建一张全 NULL 的 vtable。要做的时候，它应该长成 `.def` 里一个新的 class，配合一个自己的 exec 阶段，而不是把 `@P @*` 硬拗出第三种含义。

**它在接口上落在哪一步**——即上面四条分别要改 `exec_try_translator()` 里的哪一段——见 [04-extending.md](04-extending.md#5-编译器型aot翻译器需要的那一整块)，那一节还解释了为什么「预留一张 vtable」在这里是错的做法。

如果你要做的是这一类，本文档给不出配置步骤——请先按上面四条评估工作量。

## 6. 接入检查清单

包装器型：

- [ ] 翻译器是**宿主原生** ELF（用 `tools/xlator_fetch.py --check-guest` 或 `readelf -h` 确认 `Machine` 是宿主架构）
- [ ] 翻译器在 A20OS 的 Linux ABI 下能独立运行
- [ ] 选了匹配的 `.argv` 模板，用 shim 验证过 `argv[i]` 逐项符合预期
- [ ] 环境需求写进了 `.env`（如果有）
- [ ] **它服务的是 Linux-ABI 的 guest**（qemu-user 只有 Linux ABI；native-ABI 见第 1 节与 [04-extending.md](04-extending.md)）
- [ ] `cat /proc/a20/xlator` 里该 guest 显示 `path=…` 而不是 `path=(none)`，且 `abi=` 是你期望的那一个
- [ ] 启动日志有 `[XLATOR] <guest> (e_machine=… abi=…) → …` 一行
- [ ] 真实运行后 guest 的输出、argv、退出码都正确（`make smoke-exec-xlator` 的断言集）
- [ ] `/bin/xlate_exec native ignored` 仍然 `PASS: ENOEXEC`（确认你没有把翻译器接到一个装不下它的 ABI 上）

新增 guest 架构时追加：

- [ ] `kernel/proc/xlator_guests.def` 加了一行（ABI 列不要漏，它进查找键）
- [ ] `tools/targets-xlator.mk` 加了 `XLATOR_GUEST_CC_<name>`
- [ ] `make check-xlator-guests` 通过

设计与取舍、代码位置、内核 API 契约：[03-internals.md](03-internals.md)。扩展这条通道本身（新增 ABI、AOT、binfmt_misc 的借鉴）：[04-extending.md](04-extending.md)。