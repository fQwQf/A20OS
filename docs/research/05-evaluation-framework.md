# A20OS Native ABI：评估实验框架设计

> 本文档设计一套系统化的评估框架，用于量化验证 A20OS Native ABI 的设计目标。评估涵盖：syscall 路径性能（微基准测试）、真实工作负载性能（宏基准测试）、POSIX 兼容层开销、代码复杂度与实现质量、安全性属性的形式化验证方法。

---

## 1. 评估目标与实验设计原则

### 1.1 评估目标

| 编号 | 目标 | 度量维度 | 对应设计声称 |
|------|------|---------|-------------|
| G1 | Handle 操作开销低 | 延迟、吞吐 | "53 个简洁 syscall" |
| G2 | 统一 event queue 不劣于 epoll | 延迟、吞吐、可扩展性 | "统一等待不牺牲性能" |
| G3 | Channel IPC 性能可接受 | 延迟、吞吐、handle transfer 开销 | "Channel 替代 POSIX IPC" |
| G4 | POSIX shim 开销 < 10% | 延迟差值 | "兼容层可用" |
| G5 | Spawn 性能优于 fork+exec | 延迟、内存开销 | "spawn 替代 fork" |
| G6 | 代码复杂度低于对照系统 | LOC、圈复杂度、耦合度 | "教学内核可实现" |
| G7 | 安全不变式可验证 | 不变式违反次数 | "形式化安全保证" |

### 1.2 设计原则

**P1（可重复性）**：所有基准测试提供自动化脚本、确定性的输入生成和统计报告。

**P2（公平比较）**：与 Linux 同等功能的直接对比（如 event_wait vs epoll_wait），在相同硬件和配置下运行。

**P3（多维报告）**：报告均值、中位数、P99、标准差，不仅报告均值。

**P4（统计显著性）**：每个实验至少运行 1000 次迭代，报告 95% 置信区间。

---

## 1.5 测量方法论

### 1.5.1 计时基础设施

**计时器选择**：使用架构特定的高精度周期计数器。

| 架构 | 计时器 | 频率 | 精度 |
|------|-------|------|------|
| aarch64 | `cntvct_el0` | 与系统时钟相同 | ~10ns（1GHz） |
| riscv64 | `rdcycle` | 核心频率 | ~1ns（1GHz） |
| loongarch64 | `rdtime` | 恒定频率 | ~10ns |

**校准**：每次实验前执行 100 次空计时器读取，记录计时器开销 $\overline{t_{overhead}}$，后续所有测量值减去此开销。

**缓存预热策略**：
- 冷启动测量：每次迭代前执行 `clfush` / 缓存失效，度量包含 cache miss 的完整路径
- 热启动测量：首次迭代后不失效缓存，度量稳态性能
- 报告中必须标注测量类型（冷/热）

### 1.5.2 统计方法

**样本量确定**：基于预期效应量和变异性，使用功效分析（power analysis）确定最小样本量。

设预期效应量为 $d = \frac{|\mu_1 - \mu_2|}{\sigma}$（Cohen's d），显著性水平 $\alpha = 0.05$，功效 $1 - \beta = 0.8$：

| 效应量 $d$ | 最小样本量 $n$ | 实际采用的 $n$ |
|-----------|--------------|-------------|
| 大 ($d \geq 0.8$) | 26 | 1000 |
| 中 ($d \geq 0.5$) | 64 | 1000 |
| 小 ($d \geq 0.2$) | 394 | 1000 |

统一采用 $n = 1000$ 超过所有场景的最小需求。

**异常值处理**：
1. 计算四分位距 $IQR = Q_3 - Q_1$
2. 定义异常值边界：$[Q_1 - 1.5 \times IQR, Q_3 + 1.5 \times IQR]$
3. 报告异常值比例；若 > 5%，分析原因（调度抖动、中断等）
4. 不直接剔除异常值——同时报告包含和排除异常值的结果

**报告格式**：每个基准测试报告以下统计量：

| 统计量 | 含义 | 计算方式 |
|-------|------|---------|
| $\bar{x}$ | 均值 | $\frac{1}{n}\sum x_i$ |
| $\tilde{x}$ | 中位数 | 第 50 百分位 |
| $s$ | 标准差 | $\sqrt{\frac{1}{n-1}\sum(x_i - \bar{x})^2}$ |
| $P_{99}$ | 第 99 百分位 | 长尾性能指标 |
| $CI_{95}$ | 95% 置信区间 | $\bar{x} \pm 1.96 \times \frac{s}{\sqrt{n}}$ |
| $n_{outlier}$ | 异常值数量 | 超出 IQR 边界的数据点 |

**比较检验**：对于 A/B 对比实验（如 handle_close vs close），使用 Welch's t-test（不假设等方差）：

$$t = \frac{\bar{x}_A - \bar{x}_B}{\sqrt{\frac{s_A^2}{n_A} + \frac{s_B^2}{n_B}}}$$

报告 $p$-value 和效应量 $d$。仅当 $p < 0.05$ 且 $d > 0.5$ 时声称显著差异。

### 1.5.3 实验环境控制

**硬件**：
- 单一测试平台（报告 CPU 型号、频率、内存大小、缓存层次）
- 禁用 CPU 频率调节（设置 `performance` governor）
- 禁用超线程（如适用）
- 隔离测试核（如使用 `taskset` 绑核）

**软件**：
- 最小化后台进程（在单用户模式下运行或停止非必要服务）
- 禁用地址空间随机化（ASLR）以减少变异性
- 内核编译为 Release 模式（-O2，无调试符号）

**环境记录**：每次实验记录以下元数据：

```
{
    "timestamp": "2025-xx-xxTxx:xx:xx",
    "kernel_version": "a20os-0.x.x",
    "compiler": "gcc-xx.x.x -O2",
    "cpu_model": "...",
    "cpu_freq_mhz": "...",
    "ram_gb": "...",
    "iterations": 1000,
    "cache_policy": "hot",
    "aslr": false,
    "cpu_governor": "performance"
}
```

### 1.5.4 实验执行顺序

按以下顺序执行实验，每组实验间重新启动系统以确保干净状态：

```
Phase 1: 微基准（无外部依赖）
  1.1 Syscall 路径延迟（§2.1）
  1.2 Handle table 可扩展性（§2.2）
  1.3 权限检查开销（§2.3）

Phase 2: IPC 与事件基准
  2.1 Channel 吞吐-延迟（§3.1）
  2.2 Event queue 吞吐（§3.2）
  2.3 Spawn 延迟分解（§3.3）

Phase 3: 宏基准与应用
  3.1 I/O 密集工作负载（§4.1）
  3.2 IPC 密集工作负载（§4.2）
  3.3 POSIX shim 开销（§4.3）

Phase 4: 安全性验证
  4.1 不变式测试（§8.1）
  4.2 攻击面测试（§8.2）
  4.3 运行时不变式监测（§10.3）

Phase 5: 复杂度分析
  5.1 LOC 统计（§6.1）
  5.2 模块依赖分析（§6.2）
```

每阶段预计耗时 1-2 天（含分析和报告撰写），总评估周期约 1-2 周。

---

## 2. 微基准测试

### 2.1 Syscall 路径延迟

**目的**：度量 Native ABI 各 syscall 的从用户态调用到返回的端到端延迟。

**方法**：

```c
// 测量框架伪代码
for (int i = 0; i < ITERATIONS; i++) {
    uint64_t t0 = cycle_counter();
    int64_t result = syscall_under_test(args);
    uint64_t t1 = cycle_counter();
    record(t1 - t0, result);
}
```

使用高精度周期计数器（ARM CNTPCT 或 RDTSC），排除异常值（> 3σ），报告中位数和 P99。

**测试矩阵**：

| Syscall | 参数 | 对照（Linux） | 测量重点 |
|---------|------|-------------|---------|
| handle_close | 有效 handle | close(fd) | 纯 syscall 开销 |
| handle_dup | 同对象降权 dup | dup2(fd) | handle 表操作 vs fd 表操作 |
| handle_query | 有效 handle | fstat(fd) | 元数据查询开销 |
| handle_read | 4KB buffer | read(fd, 4KB) | I/O 路径开销 |
| handle_write | 4KB buffer | write(fd, 4KB) | I/O 路径开销 |
| path_open | 创建+打开 | openat() | 路径解析+handle 创建 |
| vm_alloc | 1 页 | mmap(4096) | 内存分配开销 |
| event_queue_create | 默认容量 | epoll_create1() | 对象创建开销 |
| event_watch | watch file handle | epoll_ctl(ADD) | 注册开销 |
| event_wait | 单事件等待 | epoll_wait(1) | 等待+返回开销 |
| msg_send | 1KB + 0 handles | pipe write | 小消息发送 |
| msg_recv | 1KB + 0 handles | pipe read | 小消息接收 |
| task_spawn | 最小 image + 3 handles | fork()+exec() | 进程创建全路径 |
| clock_get | monotonic | clock_gettime() | 纯信息查询 |

**预期结果**：
- handle 操作应与 fd 操作在同一数量级（差异 < 20%）
- path_open 可能稍慢（handle 权限检查额外开销），但不应 > 50%
- task_spawn 应显著快于 fork+exec（无需 COW 设置、无 fd 表复制）

### 2.2 Handle Table 操作可扩展性

**目的**：度量 handle 数量增长时操作延迟的变化。

**方法**：
1. 预分配 N 个 handles（N = 10, 100, 1000, 10000）
2. 测量 handle_dup、handle_close、handle_query 在不同 N 下的延迟
3. 对比 Linux fd table 在同等数量 fd 下的操作延迟

**预期结果**：A20OS 使用动态数组 + free bitmap 实现 handle table（参见 08-architecture-deep-dive.md §2），lookup/close 为 O(1)，alloc 为 O(n/64)（bitmap word 扫描，实际接近 O(1)）。Linux fd table 在旧版本中使用数组（O(n) 扫描），新版本使用红黑树。如果实现正确，A20OS 在高 handle 数量下应优于 Linux 数组实现。

**报告格式**：
```
N      | handle_dup (ns) | handle_close (ns) | handle_query (ns)
10     | ...             | ...               | ...
100    | ...             | ...               | ...
1000   | ...             | ...               | ...
10000  | ...             | ...               | ...
```

### 2.3 权限检查开销

**目的**：隔离权限检查本身的开销。

**方法**：
```c
// 测量有权限 vs 无权限的延迟差
uint64_t t_with = measure(handle_read(handle_with_R, buf));
uint64_t t_without = measure(handle_read(handle_without_R, buf));
// 权限检查开销 ≈ t_without - t_with（排除 EAGAIN 等其他因素）
```

**预期结果**：权限检查是位域的子集测试（$O(1)$），开销 < 10ns。

---

## 3. Event Queue 基准测试

### 3.1 单事件延迟

**目的**：度量从事件发生到 event_wait 返回的端到端延迟。

**方法**：
1. 创建 event queue $q$ 和 timer $t$
2. watch $t$ 到 $q$（事件：TIMER_EXPIRED）
3. 设置 timer 为 0ns 后触发
4. 调用 event_wait($q$)
5. 测量 event_wait 返回时间

**对照**：
- Linux: timerfd_create + epoll_ctl + epoll_wait
- 理想基线：纯自旋等待（理论最小延迟）

### 3.2 多事件吞吐

**目的**：度量高事件频率下 event queue 的处理吞吐。

**方法**：
1. 创建 1 个 event queue
2. 创建 N 个 timers（N = 1, 10, 100, 1000），全部 watch 到同一 queue
3. 以频率 F 触发 timers
4. 测量 event_wait 每秒可处理的事件数

**对照**：Linux epoll_wait 等同配置。

**可扩展性测试**：
```
Timers | events/sec (A20) | events/sec (Linux epoll) | ratio
1      | ...              | ...                       | ...
10     | ...              | ...                       | ...
100    | ...              | ...                       | ...
1000   | ...              | ...                       | ...
```

### 3.3 多 Event Queue 场景

**目的**：验证多 event queue 独立工作的正确性和性能。

**方法**：
1. 创建 M 个 event queues（M = 1, 2, 4, 8）
2. 每个 queue 上 watch N/M 个 timers
3. 多线程模型：每线程管理一个 queue
4. 测量总吞吐

---

## 4. Channel IPC 基准测试

### 4.1 单向吞吐

**目的**：度量 channel 单向数据传输的原始吞吐。

**方法**：
1. 创建 channel，两端分别给进程 A 和 B
2. A 循环 send，B 循环 recv
3. 消息大小：1B, 16B, 64B, 256B, 1KB, 4KB, 16KB, 64KB
4. 测量吞吐（MB/s）和每消息延迟

**对照**：
- Linux pipe
- Linux UNIX domain socket
- Linux io_uring (IOSQE_ASYNC)

**报告格式**：
```
Size  | A20 Channel (MB/s) | Linux pipe (MB/s) | Linux usocket (MB/s) | io_uring (MB/s)
1B    | ...                | ...               | ...                  | ...
64B   | ...                | ...               | ...                  | ...
4KB   | ...                | ...               | ...                  | ...
64KB  | ...                | ...               | ...                  | ...
```

### 4.2 Handle Transfer 开销

**目的**：隔离通过 channel 传递 handle 的额外开销。

**方法**：
```c
// 测量纯数据 vs 数据 + handles 的延迟差
uint64_t t_data = measure(send(ch, 1KB, 0_handles));
uint64_t t_data_h1 = measure(send(ch, 1KB, 1_handle));
uint64_t t_data_h8 = measure(send(ch, 1KB, 8_handles));
// handle transfer 开销 = (t_data_hN - t_data) / N
```

**预期结果**：每 handle transfer 开销应 < 1μs（handle table 加锁 + 条目创建）。

### 4.3 双向 Ping-Pong 延迟

**目的**：度量双向通信的 round-trip 延迟。

**方法**：
1. A 发送 1 字节到 B
2. B 收到后立即回复 1 字节
3. A 收到回复，记录 round-trip 时间
4. 重复 10000 次

**对照**：Linux pipe ping-pong, UNIX socket ping-pong。

---

## 5. 进程创建基准测试

### 5.1 Spawn vs fork+exec

**目的**：对比 A20OS task_spawn 与 Linux fork+exec 的延迟和资源消耗。

**方法**：

```c
// A20OS Native
uint64_t t0 = cycle_counter();
task_spawn(minimal_image, minimal_args, [stdin, stdout, stderr]);
// 等待子进程退出
task_wait(child);
uint64_t t1 = cycle_counter();

// Linux
uint64_t t0 = cycle_counter();
pid_t pid = fork();
if (pid == 0) { execve(minimal_image, ...); }
waitpid(pid, ...);
uint64_t t1 = cycle_counter();
```

**度量**：
- 端到端延迟（从调用到子进程退出）
- 子进程的峰值内存占用
- 内核态 CPU 时间（通过 task_status 获取）

**变量**：
- 传入 handle 数量：0, 3, 10, 50
- 父进程的 fd/handle 数量：10, 100, 1000
- 地址空间大小：空, 1MB 映射, 100MB 映射

**预期结果**：
- spawn 延迟不受父进程地址空间大小影响（不复制地址空间）
- spawn 延迟与传入 handle 数量成线性关系（O(n) handle 表复制）
- fork+exec 延迟与父进程 fd 数量和地址空间大小正相关

### 5.2 Spawn 权限降级开销

**目的**：度量 spawn 时对每个 handle 进行权限降级的额外开销。

**方法**：
```c
// 全权限 spawn
task_spawn(image, args, handles_with_full_rights);
// 最小权限 spawn（每个 handle 只保留 R）
task_spawn(image, args, handles_with_R_only);
```

**预期结果**：权限降级是 O(1) 位掩码操作（每个 handle），额外开销可忽略（< 100ns per handle）。

---

## 6. POSIX 兼容层性能

### 6.1 测试方法

通过 `liba20posix`（POSIX 兼容库）运行标准 POSIX 程序，测量兼容层引入的额外开销。

**架构**：
```
POSIX Application
      |
      v
liba20posix (shim layer)
  open()   → path_open() + fd↔handle mapping
  read()   → handle_read()
  write()  → handle_write()
  fork()   → task_spawn() + address space copy
  epoll_wait() → event_wait()
      |
      v
A20OS Native ABI
```

### 6.2 开销分解

| POSIX 操作 | Native 映射 | 额外开销来源 |
|-----------|-------------|-------------|
| open(path) | path_open(cwd, path) + fd_alloc | fd↔handle 映射表维护 |
| read(fd, buf) | fd→handle 查找 + handle_read | 1 次哈希查找 |
| write(fd, buf) | fd→handle 查找 + handle_write | 1 次哈希查找 |
| close(fd) | fd→handle 查找 + handle_close + fd_free | 1 次查找 + 1 次释放 |
| fork() | task_spawn + addr_copy + fd_table_copy | 地址空间复制 + fd 表遍历 |
| epoll_wait() | event_wait() + event 格式转换 | 事件结构体转换 |
| pipe() | channel_create + fd_alloc × 2 | channel 创建 + 2 次 fd 映射 |
| socket() | net_socket + fd_alloc | 1 次映射 |
| signal() | event_watch(signal_handle) | 信号到事件的转换 |

### 6.3 基准测试

**微基准**：逐个 POSIX 操作的开销差
```c
// 直接 Native ABI
t_native = measure(handle_read(handle, buf));
// 通过 POSIX shim
t_posix = measure(read(fd, buf));
// shim overhead = t_posix - t_native
```

**宏基准**：使用完整 POSIX 应用
1. **cp 命令**：文件复制（大量 read/write）
2. **ls 命令**：目录遍历（opendir/readdir/stat）
3. **cat 命令**：stdin→stdout 管道（read/write）
4. **简单 HTTP 服务器**：socket + epoll 模式
5. **make -j4**：大量 fork+exec + waitpid

**报告格式**：
```
Application | Native (s) | POSIX shim (s) | Overhead | Notes
cp 100MB    | ...        | ...            | X%       |
ls -R /usr  | ...        | ...            | X%       |
httpd 10k conn | ...     | ...            | X%       |
make -j4    | ...        | ...            | X%       |
```

**目标**：开销 < 10% 对大多数 I/O 密集型工作负载。

---

## 7. 内存子系统基准测试

### 7.1 vm_alloc 延迟

**方法**：
```c
for (size_t size = 4096; size <= 64 * 1024 * 1024; size *= 2) {
    measure(vm_alloc(size, PROT_RW));
}
```

**对照**：Linux mmap(ANONYMOUS)。

### 7.2 vm_map 文件映射吞吐

**方法**：
```c
// 映射 1GB 文件，顺序读取
handle = path_open(large_file, READ);
addr = vm_map(handle, 0, 1GB, PROT_READ, MAP_SHARED);
measure sequential_read(addr, 1GB);
```

**对照**：Linux mmap + 顺序读。

### 7.3 vm_share 共享内存延迟

**方法**：
```c
// 进程 A 共享 1 页给进程 B
addr = vm_alloc(4096, PROT_RW);
shm_handle = vm_share(addr, 4096, READ_ONLY);
// 通过 channel 传 shm_handle 给 B
msg_send(ch, empty_msg, [shm_handle]);
```

度量从 vm_share 到 B 成功 vm_map 的时间。

**对照**：Linux shm_open + mmap。

---

## 8. 安全性验证

### 8.1 不变式测试

**方法**：在内核中插入运行时不变式检查断言，在每次操作后验证 $\mathcal{I}$（参见 04-theory-deep-dive.md §3.1）。

```c
// 内核调试代码
void invariant_check(system_state *sigma) {
    // I1: 权限合法性
    for_each_handle(h) {
        assert(h.rights ⊆ Legal(h.obj->type));
    }
    // I3: 引用计数一致性
    for_each_object(o) {
        assert(refcount(o) == count_handles_pointing_to(o));
    }
    // I4: 对象活性
    for_each_object(o) {
        assert(refcount(o) > 0);
    }
}
```

**测试场景**：
1. 创建/关闭 10000 个 handles（验证 refcount 正确性）
2. spawn 100 个子进程，每个传递不同 handle 子集（验证权限降级）
3. 通过 channel 传递 handles 1000 次（验证 transfer 原子性）
4. 并发关闭同一对象（验证 refcount 不下溢）

### 8.2 权限提升攻击测试

**测试用例集**：

| 编号 | 攻击尝试 | 预期结果 |
|------|---------|---------|
| A1 | handle_dup(handle, ALL_RIGHTS) where handle has only READ | ACCESS 错误 |
| A2 | handle_read(handle_without_READ) | ACCESS 错误 |
| A3 | 通过 channel 发送 handle 没有 TRANSFER 权限 | ACCESS 错误 |
| A4 | msg_recv 后的 handle 权限 > 发送方授予的权限 | 不可发生（内核强制） |
| A5 | task_spawn 给子进程超过父进程持有的权限 | ACCESS 错误 |
| A6 | handle_close 后使用已关闭的 handle | BAD_HANDLE 错误 |
| A7 | vm_map handle 没有 MAP 权限 | ACCESS 错误 |
| A8 | 修改用户态 handle 值绕过检查 | BAD_HANDLE（handle 在内核表中验证） |

### 8.3 Confused Deputy 测试

**场景**：
1. 进程 A（低权限）持有 file handle $h_A$（READ only）
2. 进程 B（中权限）持有 file handle $h_B$（READ + WRITE）
3. A 通过 channel 请求 B 写入文件
4. 验证 B 只能写入自己持有 WRITE 权限的文件
5. 验证 A 无法通过 B 获得对 $h_B$ 对应文件的写权限

---

## 9. 代码复杂度度量

### 9.1 度量指标

| 指标 | 定义 | 收集方法 |
|------|------|---------|
| LOC（不含注释和空行） | 代码行数 | `cloc` 工具 |
| 圈复杂度 | 分支数 + 1 | `lizard` 工具 |
| 函数平均长度 | LOC / 函数数 | `cloc` |
| 耦合度 | 模块间调用数 | 静态分析 |
| API 表面积 | syscall 数量 | 手动统计 |
| 对象类型数 | handle 类型数 | 手动统计 |
| 权限位数 | rights 位域宽度 | 手动统计 |

### 9.2 对比框架

| 系统 | Syscall 数 | LOC（内核） | 圈复杂度均值 | 对象类型数 |
|------|-----------|------------|------------|-----------|
| Linux (v6.x) | ~400 | ~30M | ~15 | N/A |
| seL4 (验证版) | ~10 + methods | ~10K | ~8 | ~15 |
| Zircon | ~150 | ~200K | ~12 | ~25 |
| Redox | ~100 | ~150K | ~10 | ~10 |
| **A20OS Native** | **53** | **TBD** | **目标 < 10** | **14** |

### 9.3 A20OS 模块级度量

对 Native ABI 实现的每个子系统文件收集：

```
模块        | 文件             | LOC | 函数数 | 平均圈复杂度
sys_handle  | sys_handle.c     | ... | ...    | ...
sys_task    | sys_task.c       | ... | ...    | ...
sys_memory  | sys_memory.c     | ... | ...    | ...
sys_path    | sys_path.c       | ... | ...    | ...
sys_event   | sys_event.c      | ... | ...    | ...
sys_net     | sys_net.c        | ... | ...    | ...
sys_time    | sys_time.c       | ... | ...    | ...
总计        |                  | ... | ...    | ...
```

**目标**：Native ABI 实现总 LOC < 5000（不含注释）。平均圈复杂度 < 10。

---

## 10. 形式化属性验证方法

### 10.1 模型检验（Model Checking）

**工具选择**：TLA+ 或 Spin。

**建模范围**：
1. Handle table 操作（dup, close, transfer）
2. 引用计数状态机
3. Channel 消息队列（有限容量）
4. Event queue 注册和等待

**验证属性**：
```
PROPERTY 1: ∀σ. I1(σ) ∧ I2(σ) ∧ I3(σ) ∧ I4(σ)    // 安全不变式始终成立
PROPERTY 2: ∀p, n. rights(p, n, σ') ⊆ rights(p, n, σ)  // 权限单调递减
PROPERTY 3: event_wait eventually returns               // 活性
PROPERTY 4: channel FIFO order preserved                  // 消息序
PROPERTY 5: no dangling references after close            // 引用完整性
```

**状态空间控制**：
- 限制进程数为 2-3
- 限制 handle 数量为 4-8
- 限制 channel 容量为 2
- 限制 event queue 容量为 4

### 10.2 定理检验（Theorem Proving）

**工具选择**：Isabelle/HOL 或 Coq。

**证明目标**：
1. 定理 3.1（安全不变式保持）：对 SOS 规则的归纳证明
2. 定理 3.2（权限单调递减）：对 handle 生命周期的不变量证明
3. 定理 9.1（ABI 隔离）：对模块依赖图的图论证明

**策略**：先在 Isabelle 中形式化核心 SOS 规则（handle_dup, handle_close, channel_send/recv），然后对简化模型（有限类型、有限进程）完成证明。完整的端到端证明是未来工作。

### 10.3 运行时不变式监测

**方法**：在内核中可选地启用不变式检查器，在每次 syscall 返回前验证 $\mathcal{I}$。

```c
#ifdef CONFIG_INVARIANT_CHECK
#define CHECK_INVARIANTS() invariant_check(&current->system_state)
#else
#define CHECK_INVARIANTS() ((void)0)
#endif
```

**性能影响**：预期增加 < 5% 开销（每次 syscall 额外 O(|HT|) 扫描）。

---

## 11. 实验环境

### 11.1 硬件

| 配置 | 规格 |
|------|------|
| 平台 | ARM64 (A20 SoC, Cortex-A7 双核) |
| 内存 | 512MB DDR3 |
| 存储 | SD 卡 |
| 对照平台 | x86_64 (用于 Linux 对照数据) |

### 11.2 软件

| 组件 | 版本 |
|------|------|
| A20OS 内核 | 当前开发版 |
| Linux 对照 | 6.x（同等配置） |
| 测试框架 | 自定义 C 框架 + shell 脚本 |
| 统计分析 | Python + scipy |

### 11.3 测试配置

```bash
# 关闭动态频率调节
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 关闭内核地址空间随机化（如适用）
# 绑定到固定 CPU 核心
taskset 0x1 ./benchmark
```

---

## 12. 结果呈现模板

### 12.1 图表设计

**图 1：Syscall 延迟对比柱状图**
- X 轴：syscall 名称
- Y 轴：延迟（ns，对数刻度）
- 两组柱子：A20OS Native vs Linux

**图 2：Event Queue 可扩展性**
- X 轴：watch 的 handle 数量（1 → 1000）
- Y 轴：events/sec
- 三条线：A20OS event_wait, Linux epoll_wait, Linux select

**图 3：Channel 吞吐 vs 消息大小**
- X 轴：消息大小（1B → 64KB，对数刻度）
- Y 轴：吞吐（MB/s）
- 四条线：A20OS channel, Linux pipe, Linux UNIX socket, Linux io_uring

**图 4：Spawn 延迟 vs 父进程状态**
- X 轴：父进程 fd/handle 数量（10 → 10000）
- Y 轴：延迟（μs）
- 两条线：A20OS task_spawn, Linux fork+exec

**图 5：POSIX shim 开销瀑布图**
- 每个操作的 shim 层开销分解
- 颜色编码：Native 路径 | fd→handle 查找 | 格式转换 | 其他

### 12.2 表格模板

**表 1：微基准测试汇总**

| Syscall | A20 (ns) | Linux (ns) | 差异 (%) | P-value |
|---------|----------|-----------|---------|---------|
| close/dup | ... | ... | ... | ... |
| read 4KB | ... | ... | ... | ... |
| write 4KB | ... | ... | ... | ... |
| ... | ... | ... | ... | ... |

**表 2：宏基准测试汇总**

| 应用 | Native (s) | POSIX shim (s) | Linux (s) | Shim 开销 (%) |
|------|-----------|---------------|-----------|-------------|
| cp 100MB | ... | ... | ... | ... |
| httpd 10k | ... | ... | ... | ... |
| make -j4 | ... | ... | ... | ... |

---

## 13. 评估完成标准

评估实验"通过"的标准：

| 编号 | 标准 | 阈值 |
|------|------|------|
| G1 | handle 操作延迟与 fd 操作差异 | < 20% |
| G2 | event_wait vs epoll_wait 吞吐 | > 80% |
| G3 | channel vs pipe 吞吐（> 4KB 消息） | > 70% |
| G4 | POSIX shim 开销（I/O 密集） | < 10% |
| G5 | spawn vs fork+exec 延迟 | < 80%（快于 fork+exec） |
| G6 | Native ABI 实现总 LOC | < 5000 |
| G7 | 安全不变式违反次数 | 0 |

---

## 参考文献

1. McVoy, L.W. & Staelin, C. "lmbench: Portable Tools for Performance Analysis." *USENIX ATC*, 1996.
2. Soares, L. & Stumm, M. "FlexSC: Flexible System Call Scheduling with Exception-Less System Calls." *OSDI '10*, 2010.
3. Litak, D. et al. "iolat: A Microbenchmark Suite for I/O Latency." *FAST '18*, 2018.
4. Kleiman, S.R. "Vnodes: An Architecture for Multiple File System Types in Sun UNIX." *USENIX Summer*, 1986.
5. Lameter, C. "Efficient Local and Remote Atomic Operations." *LinuxCon*, 2012.
6. Kleiman, S. "Welder: A Framework for OS Benchmarking." *USENIX*, 1997.
