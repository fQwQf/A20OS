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
make check-driver-core-model      # 驱动
make check-io-progress-model      # I/O 进展
make check-concurrency-foundation # 并发
```

## 4. 代码与注释规范

### 4.1 语言

- 文档（`docs/`、`*.md`）一律中文。
- 代码注释一律英文。代码注释的既有事实也是英文（内核源码里约 98% 的注释为英文），新增注释跟随既有语言，不要引入新的混合。
- 提交信息标题用英文祈使句（见第 5 节），这与代码注释的语言选择一致。

### 4.2 格式

`.clang-format` 记录了本仓库既有的 C 风格：4 空格缩进（不使用 tab）、K&R 花括号、指针写作 `type *name`、80 列上限。它是**参考而非门禁**：CI 不运行 `clang-format`，不要为了让它满意而重排既有代码。`.editorconfig` 补充了编辑器层面的换行与缩进约定，并对 `*.s`/`*.S` 和 vendored 目录关闭自动改写（汇编的列对齐与预处理指令不能被重新排版）。

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

提交信息用于生成历史记录，请保持清晰：

- 标题使用祈使句，例如 `mm: fix COW TLB invalidation on riscv64`。
- 标题不超过 50 个字符。
- 正文说明修改原因和验证方式，可引用 issue。
- 一个提交只做一件事。

示例：

```text
fs: add refcnt helper for vnode lifecycle

Replace direct ref_count manipulation in ramfs/ext4 with
vfile_ref_init / vfile_get / vfile_put_ref_only.

Verified with:
- make check-vfs-abstraction
- make smoke-vfs-stress
```

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
