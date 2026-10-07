# 串口 RX 保真度：复现装置、基线损坏率、根因与修复后数据

日期：2026-10-07。worktree `/home/fqwqf/OS/A20OS-wt3-uart`（分支 `wt3/uart`）。
动机来自 `docs/net/net-lanes.md:1206-1216` 记录的现象：800 条控制台命令中约 3 条
在 guest 侧被相邻字符换位（`tcp_accept_test` → `tpc_` / `ctp_` / `tcp_aceptc_test`），
使 `grep -c PASS` 把「没执行」误判成「没失败」。

## 1. 复现装置：`tools/serial_fidelity.py`

逐行向 guest 喂 `echo FID%06d<payload>`（payload 为 LCG 生成的 48 字节 base62），
等每条命令自己的输出行回来再发下一条；回显行与输出行都携带同一个 token，分类器
按 token 之后的 payload 与发送值逐字节比对，区分：

| 分类 | 判据 | 含义 |
|---|---|---|
| `clean` | payload 完全一致 | 往返成功 |
| `truncated` / `transposed` / `vanished` | payload 短了 / 同字节异序 / 消失且无异物 | **RX 损坏** |
| `interleaved` | 同行被插入异物文本 | 内核打印交错（**不是** RX 损坏，不计入） |

另单列三个不进分类器、但同样毒化测量的量：`never_returned`（命令根本没回来）、
`shell_not_found`（guest 报 `inaccessible or not found`，即命令名被改写）、
`inaccessible` 计数。退出码：`corrupted>0` 或（未加 `--allow-missing` 时）
`missing>0` 都返回 1——「没执行」与「执行错」同等对待。

放大器：`--blast N`（每写 N 行不等待）、`--load CMD`（guest 后台 flood，
制造 TX 长时间占窗）、`--qemu-arg`（`-smp 4`）。

关键设计教训（写进脚本 docstring）：payload 必须 ≤48 字节。96 字节 payload 会把
行推过 mksh 的 80 列折行阈值，mksh 用 `\r`+退格重绘，看起来像损坏但实际是行编辑
输出——曾造成整轮假阳性。

## 2. 未修复基线（全部存档）

### A. 门禁配置（`-smp 4` + guest flood + paced 300 行）×3

镜像 `/home/fqwqf/a20-build/uart-unfixed`（未修复竞争代码 + 计数器探针，
`make ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4 dev-build`）。
命令与 `smoke-serial-fidelity` 门禁完全一致：
`tools/serial_fidelity.py --kernel-dir … --count 300 --load 'i=0; while [ $i -lt 200000 ]; do echo flood $i; i=$((i+1)); done' --qemu-arg=-smp --qemu-arg=4`
日志存档 `.kernel-build/smoke/serial-fidelity-baseline-{1,2,3}.{log,summary}`。

| 跑 | returned | never_returned | shell_not_found | 探针（末行） | 事件 |
|---|---|---|---|---|---|
| 1 | 43 | 257 | 1 | `reorders=1 dropped=0 poll_bytes=1 irq_bytes=2847` | log:200378 `E: mksh: ceho: inaccessible or not found`（`echo`→`ceho`），随后 60 s 等不到 FID000044 输出而停机 |
| 2 | 300 | 0 | 0 | `reorders=2 dropped=0 poll_bytes=2 irq_bytes=18974` | 2 次轮询换位事件未落到 FID 行（良性交错或落在非 FID 行） |
| 3 | 43 | 257 | 1 | 无 rx_fidelity 行（停机前未到 defect/4096 步长） | log:200312 回显行本体 `ceho FID000043YYu4L7359…`（payload 完好、命令名前两字符换位） |

**基线损坏率（A）：3 跑中 2 跑在第 43/44 条命令发生相邻字符换位，各导致其后
257 条命令完全未执行**。900 行下发中 2 条命令名被改写、514 行未返回。
这正是 `net-lanes.md` 那 3/800 的同形态复现，且比逐行 1 s 间隔的原场更早触发。

注意 `corrupted=0` 不代表无损：换位发生在 token **之前**的命令名（`echo `），
token 之后的 payload 完好，所以分类器看不到、由 `shell_not_found` + `never_returned`
兜住。这正是原症状毒化统计的方式——只数 PASS 行的脚本会把 run1/run3 的 257 条
未执行全部当成"没失败"。

### B. blast8 放大器（burst 丢字节）

同一镜像，`--blast 8 --allow-missing --count 300`（存档 `/tmp/sf-unfixed-blast8.summary`）：

```
returned=60 clean_tokens=31 corrupted=0 never_returned=240 interleaved_tokens=91 shell_not_found=5
[UART] rx_fidelity reorders=0 dropped=15268 poll_bytes=0 irq_bytes=18909
```

**到达处理程序的 18909 字节中 15268（80.7%）被 256 字节 ring 静默丢弃**。
本 ask 早前同配置一次为 `dropped=15130 irq_bytes=18900`（`/tmp/sf-instr-blast8.log:3817`），
损坏率可复现。

### C. 单核纯净镜像、paced 300（无 flood）

镜像 `/home/fqwqf/a20-build/uart-baseline`（main 纯净 `uart.c`，单核）：

| 跑 | 结果 | 存档 |
|---|---|---|
| a | 停在 FID000170，returned=169 / never=131，shell_not_found=0；日志 32048 B 止于 `# ` 提示符，第 170 行**零字节回显**、其后 `poweroff` 也无回显 | `/tmp/sf-unfixed-paced300.summary` |
| b | 停在 FID000030，returned=29 / never=271，同一形态（止于 `# `） | `/tmp/sf-unfixed-paced300-b.summary` |
| c | 全 clean：returned=300 / corrupted=0 / never=0 | `/tmp/sf-unfixed-paced300-c.summary` |

**基线损坏率（C）：2/3 跑整段停摆**——无 flood、每行等待回显的最温和配置下，
shell 之后收不到任何输入字节（连 `poweroff` 都无回显）。这与 A 的换位不同形态，
性质（uart.c RX 路径 vs `kernel/fs/devfs/tty.c` 行规程，后者属另一会话范围、
本任务未排查）**未归因**，n=3 如实记录。本 ask 前段同镜像 paced 300/800 各跑过
一次全 clean（stdout 未存档，日志 `/tmp/sf-base-paced{300,800}.log`），说明该停摆
是偶发而非确定性。

## 3. 根因（引用原始 main `278603614:kernel/drivers/char/uart.c`）

文档怀疑方向之一 `kernel/arch/riscv64/platform/timer.c`：读过后**无嫌疑**——该文件
只做节拍与调度（`timer_irq_tick()`/`proc_sched_tick()` 调用链），不碰 RX 寄存器。
证据落在 `uart.c` 两类结构性缺陷：

**(a) 256 字节 ring，满则静默丢字节。**
`:21 #define RX_BUF_SIZE 256`；`uart_rx_push()`（`:212-232`）里
`if (next != rx_tail) { 存 }` **没有 else、没有计数器**——ring 满直接丢弃且不留痕。
证据：B 的 `dropped=15268/18909`。burst 下 16 字节硬件 FIFO 溢出叠加 256 字节
ring 溢出，命令行成段消失（`shell_not_found=5`、`never_returned=240`）。

**(b) 读接收寄存器与入环不在同一临界区 → 相邻换位窗口。**
`uart_rx_push()` 自己重新加锁（`:221`），而三个任务态读取点都在**解锁之后**才
调用它：

- 站点 1 `uart_getc()`：`:265` 解锁 → `:267 arch_uart_poll_getc()` → `:269 uart_rx_push()`；
- 站点 2：`:288` 持锁读 → `:291` 解锁 → `:292 uart_rx_push()`；
- 站点 3：`:303` 持锁读 → `:306` 解锁 → `:309 uart_rx_push()`；
- 顶半部 `uart_handle_irq()`：`:388` 读 RBR → `:389 uart_rx_push()`（读与存分离）。

任务态读到字节 A 后、入环前，顶半部可以把 FIFO 里**更晚**的字节 B 先推入环，
读者随即推 A → 环内顺序 `B,A`，即相邻字符换位。证据：A 表 run1 探针
`reorders=1 poll_bytes=1`——该跑唯一一次任务态轮询读恰好与一次被计数的头部错位
同时出现，且 guest 回显 `ceho`（`echo` 前两字符换位）；run3 出现同形态换位但
探针计数为 0（探针只覆盖 `uart_getc` 的轮询站点，run3 的确切站点**未归因**，
如实交代）。run2 的 `reorders=2` 无用户可见损坏，说明计数事件不必然落到命令行上。

## 4. 修复（`kernel/drivers/char/uart.c`，当前工作区）

1. **读+入环单临界区**（`RX_LOCK_INVARIANT` 注释块 `:264-279`）：新增
   `uart_rx_push_locked()`（`:280-293`，锁内 append + 收集唤醒，Ctrl-C 字节返回 0
   交调用方解锁后处理）。三个任务态站点全部改为**持锁**读 RBR 并在同临界区内入环
   （`uart_getc` `:361-387`、`:405-417`、`:426-440`）；顶半部整个 drain 循环移入
   `rx_lock`（`uart_handle_irq` `:535-542`）；`uart_has_input()` 的 polled 板路径
   同样单临界区（`:476-490`）。由此**环序 == 寄存器序**，换位在结构上不可发生。
2. **ring 256 → 8192**（`:37`），溢出必计数 `rx_dropped++`（`:290`）——静默丢字节
   变成可观测失败。
3. **探针改为 `dropped / poll_bytes / irq_bytes`**（`uart_rx_fidelity_report()`
   `:319-331`，static；按 4096 字节步长或首次 defect 打印）。修复后 `reorders`
   计数器删除——它计的是已被结构性消灭的窗口。

## 5. 修复后实测（同规模样本，全部为本 ask 实跑）

### gate 配置 ×3（与基线 A 完全同命令，存档 `.kernel-build/smoke/serial-fidelity-fixed-{1,2,3}.*`）

| 跑 | returned | corrupted | never_returned | shell_not_found | 末行探针 |
|---|---|---|---|---|---|
| 1 | 300 | 0 | 0 | 0 | `dropped=0 poll_bytes=1 irq_bytes=16572` |
| 2 | 300 | 0 | 0 | 0 | `dropped=0 poll_bytes=1 irq_bytes=16572` |
| 3 | 300 | 0 | 0 | 0 | `dropped=0 poll_bytes=1 irq_bytes=16572` |

### blast8（与基线 B 同命令）

`returned=300 clean_tokens=600 corrupted=0 never_returned=0 interleaved_tokens=0
shell_not_found=0`，探针 `dropped=0 poll_bytes=0 irq_bytes=17931`
（日志 `/tmp/sf-fixed-blast8.log`）——基线 80.7% 字节丢弃 → **0**。

### `make smoke-serial-fidelity` 连跑 3 次

三次 rc=0 / PASS，`returned=300 corrupted=0 never_returned=0 shell_not_found=0
dropped=0`（clean_tokens 558 / 556 / 555，interleaved 2 / 1 / 1——内核打印交错，
按设计不计为 RX 损坏）。

### 汇总对比

| 配置 | 未修复 | 修复后 |
|---|---|---|
| gate（smp4+flood+paced 300） | **2/3 跑在第 43/44 条换位 + 各 257 条未返回**（900 行中 2 条命令名被改、514 行未返回） | **3/3 跑 300/300 全 clean，损坏 0、未返回 0、shell_not_found 0、dropped 0** |
| blast8（300 条） | returned=60，never=240，shell_not_found=5，**dropped=15268/18909=80.7%** | **returned=300 全 clean，dropped=0** |
| 单核 paced 300（无 flood） | 2/2 跑整段停摆（停在第 170 / 30 条，其后零回显） | 见 §4.4 |

修复后累计验证：gate ×3 + smoke ×3 + blast8 = **2100 行命令 0 损坏、0 未返回、
0 dropped**。按基线 A 的事件率（2 事件 / 900 行 ≈ 0.22%）外推，2100 行期望约
4.6 个事件，观测 0（Poisson p≈1%）——是显著改善；但样本仍是 QEMU TCG 上的
有限次运行，不是形式化证明。

## 6. 门禁与边界

- `make dev-build` rc=0（`/tmp/dev-build-final2.log`，0 error/warning）。
- `make smoke-serial-fidelity`（`tools/targets-smoke.mk` 新增，内部
  `ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4 dev-build` +
  `print-build-dir` 取目录 + `a20_resource.py -m 1G -c 4` 资源门 + 300 条断言）
  连跑 3 次全绿；断言包含：脚本退出码、`dropped=0`、`shell_not_found=0`、
  无 panic/page fault、**rx_fidelity 行必须存在且 `poll_bytes>0`**（探针被摘掉
  或任务态轮询读路径未覆盖则门禁失败，防空转）。
- 范围：只改 `kernel/drivers/char/uart.c`、`tools/serial_fidelity.py`、
  `tools/targets-smoke.mk`、`tools/smoke.py`、本文件。`uart_rx_fidelity_report()` 改为 static，
  `kernel/include/drivers/char/uart.h` 与 HEAD 一致（未改动）。未动
  `kernel/fs/devfs/tty.c`、`kernel/arch/riscv64/**`、`kernel/external/lwip/**`
  （DIVERGENCE.md 无需更新）。
- **未做**：`smoke-net-lanes-n1` 字节等价门禁未在本任务内跑——它属于 lanes/锁分片
  流的回归，由编排层统一做；本任务改动只涉及 UART 驱动与冒烟工具，不含 lane 代码。
  §2C 的停摆性质未归因（可能落在范围外的 tty.c）；真实硬件未测（仅 QEMU）。

## 7. main 集成复验（2026-10-08）

在 main 源码基线 `3d1e5b24f` 上运行 `make -j8 smoke-serial-fidelity`，门禁 PASS。
宿主日志为 `/tmp/a20-recovery-logs/smoke-serial-fidelity-final.log`；guest 日志及
汇总分别为 `.kernel-build/smoke/serial-fidelity-riscv64.log` 和
`.kernel-build/smoke/serial-fidelity-riscv64.summary`。本次结果：
`returned=300 clean_tokens=558 corrupted=0 never_returned=0 interleaved_tokens=2
shell_not_found=0`，RX 计数 `dropped=0 poll_bytes=1 irq_bytes=16572`。

`interleaved_tokens=2` 是串口日志中内核输出与会话文本交错的分类，不代表 RX 字节
损坏；300 条命令均返回，payload 逐字节匹配，`corrupted=0`。`poll_bytes=1` 也确认
任务态轮询 RX 路径确实执行，不是仅由 IRQ 接收覆盖门禁。该记录是 QEMU 集成复验，
不替代真实硬件测量。
