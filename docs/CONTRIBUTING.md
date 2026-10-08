# 贡献指南

本仓库的补丁流程尽量简单直接。

## 1. fork 与分支

```bash
git clone <你的 fork>
cd A20OS
git checkout -b fix/<简短描述>
# 或 feature/<子系统>-<描述>
```

不要直接在 `main` 分支上提交。

## 2. 本地构建

先确认能编译并运行。详细命令见 [docs/build.md](build.md)。

```bash
make ARCH=riscv64 BOARD=qemu-virt-riscv64 run
```

## 3. 通过测试门禁

提交前至少运行以下门禁，详细说明见 [docs/testing-gates.md](testing-gates.md)：

```bash
# 构建矩阵
make check-build-matrix

# 基础 bring-up smoke；timeout 可接受，不验证 syscall
make smoke-riscv64

# 广泛聚合门禁；包含构建和 QEMU runtime，可能耗时较长
make check-doc-test-gates
```

需要验证 Linux 系统调用时运行 `make smoke-abi-linux`；`smoke-riscv64` 只是 `BRINGUP=1` 启动检查。`check-doc-test-gates` 也不是快速的纯文档检查，请为其传递依赖中的构建和 QEMU smoke 预留时间。

如果修改了具体子系统，再运行对应门禁：

```bash
make check-mm-lock-model          # 内存管理
make check-vfs-abstraction        # 文件系统
make check-abi-boundary           # ABI 边界
make check-native-abi-coverage    # 新增 A20_NATIVE_SYSCALL 时（登记表/编号表/文档要对齐）
make check-abi-config-guard       # 改动 CONFIG_ABI_* 守卫写法时
make check-driver-core-model      # 驱动
make check-io-progress-model      # I/O 进展
make check-concurrency-foundation # 并发
```

### 3.1 文档里的行号引用

如果你在 `docs/` 或任何 `.md` 里写了形如「`file.c` 第 123 行」的行号引用，那么改动那个
文件之后引用就可能不再指向所说的东西。`make check-doc-drift` 的最后一步会跑
`tools/check_doc_citations.py`，对每条能解析到本仓库的引用检查行号是否还在文件
范围内：

```bash
make check-doc-drift                    # 含引用检查
python3 tools/check_doc_citations.py --verbose   # 单独跑，并列出被跳过的条目
```

它**故意**只做能精确判定的那一半：范围是否有效。它不判断"第 123 行是否还在说
那句话"——那需要读者而不是正则；假装能判定，只会得到一个天天喊狼来了、
最后被人加 `--no-verify` 关掉的门禁。三类条目会被跳过并计数，不会让门禁失败：
指向仓库外的上游源码（mesa、virgl、wlroots 等）、形如 `[foo.c:122] 某条日志` 的
**引用的程序输出**（那是证据不是引用），以及 basename 在树内不唯一的引用（裸写的
`trap.c` 无法区分 `kernel/core/trap.c` 和八个 arch 的 `trap.c`，猜错会让门禁误报）。

`tools/check_doc_citations.py` 顶部的 `KNOWN_MOVING` 列出暂时无法校验的文档，
每条都写明原因（当前是 `feat/mm-complete` 正在重写 `kernel/mm/`，其中所有行号都会
再动一次）。该分支合并后请从列表中删除对应条目——那一刻这些行号才稳定下来。

## 4. 代码与注释规范

### 4.1 语言

- 文档（`docs/`、`*.md`）一律中文。
- 代码注释一律英文。代码注释的既有事实也是英文（内核源码里约 98% 的注释为英文），新增注释跟随既有语言，不要引入新的混合。
- 提交信息标题用英文祈使句（见第 5 节），这与代码注释的语言选择一致。

### 4.2 格式

`.clang-format` 记录了本仓库的 C 风格：4 空格缩进（不使用 tab）、K&R 花括号、指针写作 `type *name`、80 列上限。它由 `make check-format` 强制执行，但方式是**棘轮（ratchet）而不是"整棵树干净"的断言**。

具体语义：门禁只扫描第一方 `.c`/`.h`（`kernel/` 与 `user/` 下的源码与头文件），并且**排除** `kernel/external/`、`user/external/`、`user/build/`、`build/`、`.kernel-build/`、`docs/` 以及生成文件 `kernel/fs/rootfs_overlay.c`——vendored 树不是本项目可以重排的东西，量也大到会淹没门禁本身。门禁报告哪些文件不符合 `.clang-format`，但**从不修改任何源文件**。

不合规的文件里，有一份是**历史存量**：`tools/clang-format-baseline.txt` 记录了门禁落地时就已经不符合的 843 个文件（共 995 个在扫描范围内）。门禁只对**不在**这份名单里的不合规文件失败。因此它保证的是"风格不再继续劣化"，而不是"现有代码已符合本文件"——后者并不成立，最大的一处差距是 `BreakBeforeBraces: Attach` 与树里把函数左花括号单独放一行的实际写法。

如果你要缩小这份名单：改好文件，让它合规，然后用 `make format-baseline` 重写名单并把 diff 一起提交。名单只应缩小，不应扩大——扩大它等于放弃门禁，而这个 diff 本身就是"放弃了什么"的记录。名单里已经合规的条目会以 `note:` 提示，可以随时清掉。

门禁对缺失的工具是**显式跳过**而非静默通过，也非硬失败：没装 `clang-format` 时它打印 `SKIP` 并说明如何安装（`apt-get install clang-format`，或 `make check-format CLANG_FORMAT=/path/to/clang-format`）后以 0 退出。之所以不硬失败，是因为一个贡献者本地没有这个工具，不应该被一个他无法修复的红构建挡住；但静默通过同样不可接受——那等于门禁不存在。

注意 `check-format` **不在** `make check` 里。`make check` 复刻的是 CI 的 `toolchain-gates` job，而 CI runner 并不安装 `clang-format`；把它折进去会让 `make check` 变成一个 CI 无法复现的东西。请显式运行它。

`.editorconfig` 补充了编辑器层面的换行与缩进约定，并对 `*.s`/`*.S` 和 vendored 目录关闭自动改写（汇编的列对齐与预处理指令不能被重新排版）。

### 4.3 什么值得写注释

内核代码的注释密度本身不是目标。判断标准是：**注释是否提供了从代码本身读不出来的信息**。值得写的：

- 为什么这样设计，而不是这样：权衡、被否决的方案、硬件或 ABI 约束。
- 不变量与前置条件：锁序、锁的前置/后置条件、内存序要求、调用方必须持有或不得持有的东西。
- 代码形状藏起来的值：魔数的推导、容量/尺寸常量、寄存器位域的含义、线格式字节偏移。
- 踩过的坑：真实 bug 的根因、寄存器读回值异常时的排查线索。
- 契约块：跨文件、跨子系统的不变量用 `NAME_CONTRACT:` 命名块集中写清（`COW_FAULT_TLB_CONTRACT`、`LWIP_NO_THREAD_PROGRESS_CONTRACT`、`SCHEDULER_CONCURRENCY_PREREQS` 等）。门禁会 grep 这些 token，不要随手改名或删除。

不值得写的（应当删除）：

- 复述标识符或紧邻代码行：`// 分配一个物理帧` 写在 `frame_alloc()` 上方。
- 逐行叙述代码在做什么。
- 指向已经搬走的函数的节标题注释（函数拆分后遗留的孤儿注释是最常见的缺陷类型）。
- 纯装饰：`====` 分隔线、ASCII art、每文件重复的许可横幅。

### 4.4 文件头

每个自研 `.c` / `.h` 文件以简短文件头开始，说明这个文件负责什么以及为什么与相邻文件分开，而不是罗列函数。当前内核有约 79% 的文件缺少文件头，这是最值得补齐的一项。

### 4.5 锁序注释

全局锁层级定义在 `kernel/include/core/lock.h`，完整顺序见 [drivers/guide/lock-order.md](drivers/guide/lock-order.md)。驱动的锁序注释用 `LOCK_ORDER:` 前缀标注，写的时候**只写相对顺序或被保护的不变量**：

```c
/* LOCK_ORDER: rx_lock is released before acquiring proc_lock in the Ctrl-C
 * path.  The dump/signal helpers acquire proc_lock on their own. */
```

不要写只重复下一行 `spin_lock_irqsave()` 的形式（`/* LOCK_ORDER: acquire rx_lock to push a character. */`）。这种模板化注释已经在一次清理中从 53 处减到 18 处，剩下的都是承载真实约束的；新增时不要复制被删掉的那种写法。

### 4.6 ABI 与寄存器

- ABI 层（`kernel/abi/`）手工解码的结构体必须给出**字节偏移表**并注明参考的 uapi 头文件。参考对象是调用方 userspace 里的结构体：对 Linux 兼容层来说是 musl/glibc 的 `shmid_ds`、`semid_ds`、`msqid_ds` 等，**不是**内核内部的 `shmid64_ds`。
- 手写解码的 MMIO 寄存器位域必须注明每一位选中了什么，以及写错会导致什么可观察的后果。位域常常是**编码**而非字面值（STM32 的 `PLLMUL` 就是 `bits + 2` 且 14 是特例）。

## 5. 提交信息风格

标题统一使用 `type(scope): description`，以英文祈使句说明这个提交改变了什么。描述首词小写，保留 `QEMU`、`VMA`、`RISC-V`、函数名和路径的原有拼写；不加句号。标题以 72 字符内为宜，最长 100 字符。`scope` 使用小写子系统名，例如 `mm`、`net`、`hyp`、`build` 或 `history`。

| type | 用途 |
| --- | --- |
| `feat` | 增加可用行为或接口 |
| `fix` | 修正已有行为、错误或回归 |
| `refactor` | 调整实现结构，保持外部行为 |
| `perf` | 改善性能或资源开销 |
| `docs` | 修改文档 |
| `test` | 增补或修正测试、门禁及基准工具 |
| `build` | 修改编译、依赖或打包流程 |
| `ci` | 修改持续集成流程 |
| `chore` | 其他仓库维护 |
| `revert` | 撤销已有改动 |
| `merge` | 描述多父提交集成的功能或修复 |

类型取决于主要改动，不因补了测试就把修复写成 `test`，也不因同时改了 Makefile 就把功能写成 `build`。scope 使用子系统名称，不把 `fix`、`feat` 等类型再混入 scope。

正文与标题空一行。需要正文时，解释修改原因、行为变化和重要限制；验证部分只写实际执行的命令及结果。测试未执行时如实说明原因。完整日志、诊断过程和长期设计记录放在文档中，提交信息概括结论并给出路径。一个提交围绕一个可独立审阅的目的；避免 `update`、`fix bugs`、`WIP` 或阶段编号充当全部说明。

`Co-authored-by` 只用于实际参与该补丁的共同作者，不自动添加模型或工具名称。已有真实作者的署名应保留，不把工具使用记录冒充共同作者。

示例：

```text
fix(mm): flush stale COW translations on RISC-V

Invalidate the previous writable translation before exposing the
read-only COW mapping to another hart.

Validation:
- make check-mm-lock-model
- make smoke-mm-pt-race
```

示例中的验证命令仅说明写法，应替换为该提交实际执行的检查。历史提交信息规范化与验证工具见 [历史迁移工具](../tools/history_rewrite/README.md)。

## 6. Pull Request

1. 将分支推送到你的 fork。
2. 在仓库页面创建 Pull Request。
3. PR 描述中写明：
   - 修改了什么
   - 为什么需要改
   - 运行了哪些门禁
   - 是否关联 issue

CI 通过后，维护者会进行代码审查。请根据评论修改，并确保门禁持续通过。

## 7. 提问

有问题请先查看 [docs/testing-gates.md](testing-gates.md) 和 [docs/build.md](build.md)。若仍未解决，请创建 GitHub Issue，标签选 `question`，并附上相关命令和日志。

## 注意

- 不要直接修改 `docs/testing-gates.md` 或 `docs/external-dependencies.md` 中的契约字符串，除非你的改动确实触及这些契约。
- 不要把 `ALLOW_UNVERIFIED_SMP=1` 作为默认门禁参数。只有 RISC-V64、AArch64、LoongArch64 和 x86_64 的同名 QEMU virt 板在 `Makefile` 的 SMP 白名单中；其他平台仅可在专门处理 SMP bring-up 的变更中使用该开关。
- 提交前请执行 `make clean` 再运行一次关键门禁，避免旧产物造成误判。
