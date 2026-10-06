# 驱动锁顺序契约

`kernel/include/core/lock.h` 给出全局锁顺序；驱动私有锁的局部顺序、例外与嵌套规则都在这里。

## 范围

覆盖的驱动：

| 驱动 | 文件 | 私有锁 |
|--------|------|--------------|
| virtio-blk | `kernel/drivers/block/virtio_blk.c` | `inst->lock` |
| virtio-net | `kernel/drivers/net/virtio_net.c` | `net->lock` |
| UART | `kernel/drivers/char/uart.c` | `rx_lock` |
| PTY | `kernel/drivers/char/pty.c` | `g_pty_alloc_lock`、每个 pair 的 `lock` |
| loop | `kernel/drivers/block/loop.c` | `g_loop[i].lock` |
| DW SDIO | `kernel/drivers/block/dw_sdio.c` | `g_sdio.lock`（mutex，`lock` → `irq_lock`）；`g_sdio.irq_lock` 只保护中断 latch |
| StarFive GMAC | `kernel/drivers/net/starfive_gmac.c` | 无 |
| Loongson-2K GMAC | `kernel/drivers/net/ls2k_gmac.c` | 无 |
| AHCI | `kernel/drivers/block/ahci.c` | sleepable `port->lock` mutex |
| VirtIO-SCSI | `kernel/drivers/block/virtio_scsi.c` | sleepable `dev->lock` mutex |
| E1000 | `kernel/drivers/net/e1000.c` | `nic->lock` |
| RTL8139 | `kernel/drivers/net/rtl8139.c` | `nic->lock`：RX/TX ring、描述符、游标与统计；无局部顺序（lwIP 投递在锁外，见 `rtl8139.c:242` 的 `LOCK_ORDER` 注释） |
| VMSVGA/SVGAv3 | `kernel/drivers/gpu/vmsvga.c` | `svga->lock` |
| DRM/KMS/virtio-gpu 3D | `kernel/drivers/gpu/drm.c` | `g_drm_store.lock`；`g_vblank.lock` |
| VirtIO-GPU transport | `kernel/drivers/gpu/virtio_gpu.c` | `inst->command_lock`（另有 `gpu_core.c` 的 `g_gpu_lock`，spinlock） |
| VirtIO input | `kernel/drvmod/examples/vinput.c (inst->lock 在模块内)` | `inst->lock` |
| virtio-console | `kernel/drivers/char/virtio_console.c`（generic 模块 `virtio-console.a20drv`，锁在模块内） | `g_vport.lock`：字节环、控制台镜像缓冲、两个 last_used 游标与计数器；无局部顺序（镜像投递在锁外） |
| virtio-rng | `kernel/drivers/char/virtio_rng.c`（generic 模块 `virtio-rng.a20drv`，锁在模块内） | `g_vrng.lock`：字节环、每槽 posted 标志、last_used 游标与计数器；无局部顺序（熵池投递与有界等待都在锁外） |
| xHCI | `kernel/drivers/usb/host/xhci.c`（generic 模块 `xhci.a20drv`） | `xhci->lock`：HCD 全部 ops 与 IRQ handler 都取它；**锁下不得等待硬件**，同步传输靠 `xfer_busy` 所有权标志串行化 |
| USB HID | `kernel/drivers/usb/class/usb_hid.c` | 每接口 `h->lock`；局部顺序 `h->lock -> xhci->lock`（单向） |
| USB storage | `kernel/drivers/usb/class/usb_storage.c` | 每实例 `st->lock`；局部顺序 `st->lock -> xhci->lock`（单向）；`msc_command()` 仍跨同步 bulk wait 持有 `st->lock`，是现存违规 |

## 全局顺序摘要

`kernel/include/core/lock.h` 中的全局顺序为：

```text
cg_node.lock -> proc_lock -> runq_lock -> pfa.lock
proc_lock -> park_lock
proc_lock -> runq_lock
proc_lock -> signal_state.lock
park_lock -> signal_state.lock
park_lock -> g_wait_timer_lock
park_lock -> runq_lock
proc_lock -> files_struct.lock -> VFS global-file/vnode locks
proc_lock -> mm_struct.lock
proc_lock -> a20_handle_table.lock
driver registry/IRQ locks -> device-private locks
g_lwip_lock -> g_net_lock
g_lwip_lock -> virtio-net nonblocking send/recv paths only
```

`g_lwip_lock -> g_net_lock` 是全局允许顺序的上界；当前网络实现采用更严格规则：二者不同时持有，lwIP callback 通过原子 ring 把工作转交给只持有 `g_net_lock` 的 bottom-half。驱动侧仍会在 `g_lwip_lock` 外层进入非阻塞设备私有锁。

对驱动而言，这意味着：

- 设备私有锁永远是最内层锁。
- 持有设备私有锁时，绝不能获取 `proc_lock`、`park_lock`、`g_wait_timer_lock`、`files_struct.lock`、VFS 锁、`mm_struct.lock`、`a20_handle_table.lock` 或 `runq_lock`，除非下文的具体局部顺序记录了该例外。
- 持有 spinlock 时绝不能阻塞、睡眠或调入调度器。
- 持有设备锁或 lwIP 锁时，绝不能调用内存分配、VFS 或 scheduler 路径，除非 callee 明确记录为非阻塞。

## 各驱动契约

### virtio-blk

`virtio_blk_inst_t.lock`（`inst->lock`）每实例一个，保护请求 slot（`inst->req[]`）、descriptor/available/used ring 状态、`inst->in_flight`、`inst->status[]`、`inst->req_hdr[]` 和 `blk->last_used`、`blk->desc_idx`。它没有局部顺序：`inst->lock` 从不与其他内核锁嵌套。

规则：

- 提交、完成投递和请求分配都在 `inst->lock` 下运行。完成可以由中断驱动，也可以由轮询驱动，两者取的是同一把锁、走的是同一条排空路径，所以锁契约不变。
- 完成路径在 `inst->lock` 下通过 `wait_queue_collect_all()` 把 task 引用和 `wait_seq` 转移到局部 wake queue；释放 `inst->lock` 后才调用 `proc_wake_q_flush()` 进入 scheduler。
- 当没有可用 slot 时，`virtio_blk_rw()` 在 yield CPU 前释放锁。
- 不要在 `inst->lock` 下调用 VFS、`kmalloc` 或阻塞式 scheduler 函数。

中断模型是三级阶梯（`virtio_blk.c` 的 probe 与 `virtio_blk_irq_handler`）：每队列一个 MSI-X 向量优先，其次共享 INTx 线，最后才是轮询。每级只在前一级失败时才进入，所以提供了 MSI-X 的设备不会再落到那条电平触发的共享线上。`inst->irq_registered` 是"完成走中断"的唯一判据，`.poll` 只在它为 0 时才是进度来源。这里没有 cmdline 开关去强制轮询，阶梯完全由 `request_irq()` 成功与否决定。MSI-X 部分注册成功时会把已注册的那些 `free_irq()` 掉再退回 INTx，不留下半接管状态。

注意 virtio-blk 与 AHCI/E1000 的一个差别：`/proc/a20/perf` 里 `virtio_blk_*` 的计数器不区分完成是由中断还是轮询投递的，所以从外部看不出中断路径是否真的在工作。这是已知缺口。

### virtio-net

`virtio_net_inst_t.lock`（`net->lock`）每实例一个，保护 TX/RX descriptor ring、`tx_busy[]`、`rx_buf[]`、`tx_buf[]`、队列索引（`last_used`、`avail->idx`）和统计信息（`rx_packets`、`tx_packets`、`rx_drops`、`tx_drops`）。局部顺序是 `g_lwip_lock -> net->lock`：

- lwIP raw-API send 路径在持有 `g_lwip_lock` 时调用。
- `virtio_net_send()` 且 `nonblock == 1` 是唯一可以在 `g_lwip_lock` 下运行的 virtio-net 入口点。
- nonblock 路径绝不睡眠等待 TX 完成。它提交 descriptor 后立即返回。
- 阻塞 send/recv 路径（`nonblock == 0`，或 `virtio_net_recv()`）不得在 `g_lwip_lock` 下调用，因为它们可能睡眠。

由此得到的规则：

- 绝不跨 scheduler wait 持有 `net->lock`。
- 持有 `net->lock` 时绝不调用 lwIP 函数（顺序是 lwIP 外层、net 内层）。
- `virtio_net_class_poll()` 只获取 `net->lock`。它内部调用的 `virtio_net_complete_tx_locked()` 是唯一在 `net->lock` 下清 `tx_busy[]` 的地方，由 TX 完成中断和该 poll hook 两条路径触达。

### UART

`rx_lock` 是单个全局 spinlock，保护接收环形缓冲区（`rx_buffer`、`rx_head`、`rx_tail`）、`rx_waiter` 和 `tty_foreground_pgid`。它没有局部顺序，`rx_lock` 不与 `proc_lock` 嵌套：

- 普通 RX 处理只在 `rx_lock` 下触碰环形缓冲区和前台 PGID。
- RX 唤醒在 `rx_lock` 下 collect wait entry，释放锁后 flush wake queue。
- Ctrl-C 路径不持有 `rx_lock`；task dump、带引用 PID 查询和信号发送各自在驱动锁外获取所需的进程锁。

因此所有 UART 路径在持有 `rx_lock` 时都不得获取额外锁。`uart_getc()` 使用 prepare → 锁内重查/link → unlock → commit 的 Park/Wake 协议，并在阻塞当前任务前释放 `rx_lock`。

### PTY

有两个锁。`g_pty_alloc_lock` 是用于 pair 分配和初始设置的全局 spinlock，`g_ptys[idx].lock` 是某个 pair 所有运行时操作的 per-pair spinlock。

`g_pty_alloc_lock` 保护分配期间的 `in_use` 标志，以及 `m2s_buf` / `s2m_buf` 分配和初始字段设置。per-pair `lock` 保护环形缓冲区（`m2s_*`、`s2m_*`）、`master_refs`、`slave_refs`、`locked`、`master_nonblock`、`slave_nonblock`、`master_waiting`、`slave_waiting` 和 termios 状态，以及窗口大小（`ws_row`、`ws_col`）。

`g_pty_alloc_lock` 与 per-pair lock 从不嵌套；阻塞 read 入队时使用 `per-pair lock -> wait-queue lock`。

- `pty_alloc()` 获取 `g_pty_alloc_lock`，用 `kmalloc` 分配缓冲区，初始化 pair，然后释放 `g_pty_alloc_lock`。分配期间不持有 per-pair lock。
- 所有 read/write/ioctl 路径只获取 per-pair lock。
- master/slave close 路径获取 per-pair lock 来更新各自的引用计数；只有两端引用和等待者都清零后才释放缓冲区并清除 `in_use`。
- 阻塞 read 在持有 per-pair lock 时把任务加入对应 wait queue，再释放 pair lock 并调度；write 和对端 close 会唤醒该队列。

由此得出的规则：

- 不要跨 VFS 操作持有 per-pair lock。等待队列操作只按 `per-pair lock -> wait-queue lock` 的局部顺序短暂嵌套。
- `g_pty_alloc_lock` 在 `kmalloc` 期间保持持有，`pty_maybe_free_locked()` 也在 per-pair lock 下执行 `kfree`。两者都是现存例外；新的代码不得复制这种模式。

### loop

`g_loop[i].lock` 每个 loop device 一个，保护 `in_use`、`backing_vf` 和 `backing_size`，没有局部顺序。

- `loop_set_fd()` 在获取 loop lock 之前先从 VFS 获得 `vfile_t` 引用和大小。
- `loop_clr_fd()` 在 loop lock 下清除状态，然后释放锁，再释放 `vfile_t` 引用。
- `loop_dev_read()` 和 `loop_dev_write()` 在 loop lock 下复制 `backing_vf` 和 `backing_size`，释放锁后再无锁调用 backing file 的 `lseek`/`read`/`write` 操作。

loop lock 从不跨 backing-file VFS 操作持有，loop lock 下也不发生内存分配。

### DW SDIO

该驱动有两个私有锁，**任何路径都不同时持有**：

- `g_sdio.lock` 是可睡眠的 `mutex_t`，串行化命令/数据寄存器序列与 `g_sdio` 的 `ready/rca/sectors`。它必须可睡眠，因为中断完成路径要在锁内 park 到 `wait_queue_t`（见下）。
- `g_sdio.irq_lock` 是 `spinlock_t`，只保护 `g_sdio.irq_status` 这一把中断 latch。只在中断顶半部与轮询喂 latch 处持有，绝不跨寄存器访问。

单向顺序（只在持有 `lock` 时才允许再取 `irq_lock`；反向禁止）：

- 中断模式与轮询模式都从 `sdio_xfer_block()` 进入 `mutex_lock(&g_sdio.lock)`；`dw_sdio_class_read/write()` 不自己加锁。
- `sdio_wait_cond()`（合并了原 `sdio_wait_cmd`/`sdio_wait_data_idle`）先自旋 `SDIO_IRQ_PRE_POLL_US`，仍不满足才 park（`proc_park_prepare/commit` + `wait_queue_link/unlink`），chunk 上限 `SDIO_PARK_CHUNK_MS`；每次 park 前重新检查 latch、`stopped` 与 deadline，所以不会睡过中断。
- `sdio_irq_handler()` 只拿 `irq_lock`、写 latch、write-1-to-clear RINTSTS、`wait_queue_collect_all`，然后**在释放 `irq_lock` 之后**才 `proc_wake_q_flush()`：唤醒者不能在自旋锁下排进调度。
- park 前用 `sdio_can_park()`（`irq_registered && !poll_only && proc_current() != NULL`）守门：probe 在 `kernel_main` 里跑，`proc_current()` 返回 NULL，所以卡枚举（`dw_sdio_init_dev`）全程轮询，INTMASK 始终为 0。这条约束不能删——在 spinlock 下 park 会死锁。
- 自旋与 park 都有超时上限（`SDIO_CMD_TIMEOUT_MS` / `SDIO_SECTOR_TIMEOUT_MS`），不会无限等待。

锁内只做寄存器轮询、数据搬运与 park/唤醒，不分配、不调用 VFS。`sdio_downgrade_to_poll()` 在 `lock` 内调用 `free_irq()`：此时没有 in-flight handler 依赖本锁，且 `free_irq` 自己会等待在途 handler 完成。

### StarFive GMAC

每实例 `starfive_gmac_priv_t.lock` 是 spinlock，保护 TX/RX descriptor ring 与 `tx_busy/rx_busy`。

- 实例池 `g_gmac_insts[]`（最多 4 个）按 MMIO 基址查找，probe 时分配。
- `starfive_gmac_send()`、`starfive_gmac_recv()`、`starfive_gmac_poll()` 在锁内完成寄存器轮询路径并检查 descriptor ownership bit。
- 锁内不做分配、VFS 或回调网络栈；`mdelay()` 只出现在 init/PHY 阶段（锁外）。

IRQ 驱动的完成路径必须复用同一锁并在此记录顺序。多实例已支持，但数据面仍是轮询，未接 IRQ。

### Loongson-2K GMAC

每实例 `ls2k_gmac_priv_t.lock` 是 spinlock，保护 TX/RX descriptor ring 与 `tx_busy/rx_busy`。

- 实例池 `g_ls2k_gmac_insts[]`（最多 4 个）按 MMIO 基址查找，probe 时分配。
- `ls2k_gmac_send()`、`ls2k_gmac_recv()`、`ls2k_gmac_poll()` 在锁内完成寄存器轮询路径并检查 descriptor ownership bit。
- 锁内不做分配、VFS 或回调网络栈。

IRQ 驱动的完成路径必须复用同一锁并在此记录顺序。数据面仍是轮询，未接 IRQ。

### AHCI

`ahci_port_t.lock`（`port->lock`）是 sleepable mutex，保护单端口 command slot、command table 和共享 transfer buffer。它没有局部顺序，类 read/write 获取该锁后串行完成分块 I/O。

等待模型：mutex 串行化整个命令。提交后先做有界短轮询；有 IRQ 时在仍持有 sleepable mutex 的情况下 park 到 wait queue，top-half 不获取 mutex，只记录状态并唤醒；无 IRQ 时回退有界轮询。这里允许睡眠是因为它不是 spinlock，但仍限制每端口一个 in-flight 命令。

**AHCI_IRQ_MODEL**（完成路径中断化后）：

- 驱动默认走中断路径。probe 时先 `request_irq()`，成功后才写 `PxIE` 和
  `GHC.IE`；`a20.ahci.poll=1`（或平台没有可路由的 IRQ）时两条都不写，
  保持纯轮询。**顺序不能反**：PxIE 先于 handler 打开会让一个无人清理
  源的 level 线打爆 CPU。
- `port->last_is` 是唯一跨越 IRQ 边界的共享状态：top-half 先把它发布出去
  再写清 `PxIS`，等待者必须还能看到已被确认的 `TFES`。因此它用
  `__atomic_load_n/store_n` 的 acquire/release 访问，而不是 `volatile` ——
  在弱序 ISA 上 `volatile` 不提供任何顺序，而 handle 里 publish
  `last_is` 与紧随其后的 `proc_wake_q_flush()` 之间正好存在这个窗口。
- top-half 只做：读 `PxIS` → 发布 `last_is` → 写清 `PxIS` →
  `wait_queue_collect_all()` → **在锁外** `proc_wake_q_flush()`。
  它不获取 `port->lock`，不 park，不分配，也不碰调度器内部锁。
- 等待者 park 前后各重查一次 `PxCI`/`PxIS`/`last_is`，所以"完成先于
  入队"不会睡过去；park 分片有上限（`AHCI_PARK_CHUNK_MS`），丢唤醒只会
  退化成重查，不会变成停滞。
- 计数：驱动自有计数器经 `/proc/a20/perf` 暴露
  （`ahci_commands` / `ahci_irq_completions` / `ahci_irq_wakeups` /
  `ahci_park_rounds` / `ahci_poll_completions` / `ahci_errors`）。中断路径和
  轮询回退从外部看都"盘能用"，只有把两者分开计数才能区分。

仍存在的限制：单 controller / 单 port / 单 command slot；`port->last_is`
是单字，所以同一端口一次只允许一个 in-flight 命令；驱动没有 MSI-X 路径，
只走 INTx 或轮询。

### VirtIO-SCSI

`virtio_scsi_dev_t.lock`（`dev->lock`）保护 request queue descriptor、avail/used index、共享 request/response、单命令 buffer 和计数器（`commands`/`flushes`/`timeouts`/`irq_completions`/`spin_completions`）。它没有局部顺序；`dev->lock` 是 sleepable mutex，当前每控制器只有一个同步 in-flight 命令。

**已知限制**在于每控制器当前只有一个同步 in-flight 命令，且使用混合完成窗口：先在锁内短时自旋轮询（`VIRTIO_SCSI_HYBRID_PRE_POLL_US`），随后 park 等待（IRQ 注册失败时回退纯轮询）。睡眠锁 `dev->lock` 不被 IRQ top-half 持有，因此轮询窗口不会阻塞中断路径。未来多队列实现必须改为 per-request 状态与 completion。

**VIRTIO_SCSI_IRQ_MODEL**（完成路径中断化后）：

- probe 按 virtio_blk 同样的三级阶梯选路：先 `msix_prepare()` + `request_irq(msix_base)`
  并 `msix_arm()`（requestq 一个 vector；controlq/eventq 建了但当前不驱动）；
  失败则 `dev->vt.irq >= 0` 时按 `shared_irq` 注册 `IRQF_SHARED` 的 INTx；
  都失败、或 `a20.virtio-scsi.poll=1`、或平台没给出 IRQ，才落到纯轮询。
  `dev->irq_mode` 记录实际走的那一级，只有 `A20_BLK_IRQ_MSIX` 才写 `INTERRUPT_ACK`。
- top-half（`virtio_scsi_irq_handler`）不获取 `dev->lock`，不读 descriptor/avail/used，
  只做三件事：`irq_count` 用 `__atomic_fetch_add(RELAXED)` 累加 → 读
  `VIRTIO_MMIO_INTERRUPT_STATUS` 并在非 legacy 时写 `INTERRUPT_ACK` →
  `wait_queue_collect_all()` 收集后**在锁外** `proc_wake_q_flush()`。
  `irq_count` 是锁外原子而不是 `dev->lock` 下的普通自增，正是为了让 top-half
  完全不碰睡眠锁；它只用于统计，不参与完成判定。
- top-half **不**按 `isr == 0` 提前返回。MSI-X 下一条 used entry 不会置
  `INTERRUPT_STATUS`，沿用 virtio_blk 那类 "无 ISR 即返回" 的写法会让
  MSI-X 的 park 等待永远收不到唤醒。
- 等待者（`virtio_scsi_command`）在 link 进 wait queue 后重查一次 used index，
  若发现完成发生在 park 之前则记 `irq_completions`，否则记 `spin_completions`；
  分片 park 有上限，丢唤醒只退化成重查。
- 计数经类接口 `A20_BLK_IOCTL_GET_STATS` 暴露（**不用** `/proc/a20/perf`：
  perf 是全局计数器，无法把数字归属到被测的那一个控制器）。读计数在
  `dev->lock` 下取快照。
- remove 回收：MSI-X 路径 `free_irq(dev->vt.msix_base)` + `msix_teardown()`；
  否则 `free_irq(dev->vt.irq)`；随后清 `irq_registered`/`irq_mode`。

### E1000

`e1000_device_t.lock`（`nic->lock`）保护 RX/TX descriptor、buffer、`rx_next/tx_next/tx_done`
和驱动私有的中断计数。局部顺序是 `g_lwip_lock -> nic->lock`。send/recv/poll 可由 lwIP 在外层锁下
调用，驱动锁下不得回调 lwIP、分配或睡眠。

数据面改为中断驱动后，IRQ top-half 的锁用法必须按下面记录，否则它就是未登记的嵌套：

- `e1000_irq_handler()` 先取 `nic->lock` 做 `e1000_reclaim_tx_locked()`，**释放锁之后**再去取
  `g_lwip_lock` 排空 RX。因此两条锁之间不存在任何方向的嵌套，不与 `g_lwip_lock -> nic->lock`
  冲突，也没有为它新增局部顺序条目。
- `e1000_poll()` 只取 `nic->lock`（读 ICR + reclaim）。它**不**排空 RX：排空是 lwIP 栈在
  `g_lwip_lock` 下做的，poll 路径再排一次会把同一个包投递两次。
- ISR 不跨 `nic->lock` 等待硬件，也不在任一锁下分配或睡眠。

已知限制：`a20_lwip_process_netif_irq_locked()` 按 ring 上限一次性排空，没有 NAPI 式的每次
IRQ 处理预算，所以 ISR 在包量很大时会长时间占用 `g_lwip_lock`。加预算要改
`kernel/net/lwip_stack.c` 的共享函数（virtio-net 同样调用它），不是驱动内的改动。

### VMSVGA/SVGAv3

`vmsvga_device_t.lock`（`svga->lock`）保护 command buffer header、command submission 和 update 序列，没有局部顺序。`flush` 在锁内提交并轮询短 command completion；不得在此锁下执行 framebuffer 映射、VFS 或用户 copy。

### DRM/KMS/virtio-gpu 3D

`g_drm_store.lock`（`drm.c`）保护全部 device-global 状态：GEM 表
`g_gems[]`、framebuffer 表 `g_fbs[]`、CRTC 绑定 `g_crtc`、GEM name 表、
PRIME 表、virgl 资源/上下文 id 计数器，以及 EDID 的一次性缓存。这些表按
设计是设备全局的——一个 open 建立的 buffer 必须对另一个 open 可见——所以
wlroots backend 与 renderD128 上的 GBM client 会并发访问它们。

局部顺序：

- `g_drm_store.lock` 是设备私有锁，永远是最内层锁。
- **锁只保护表，不保护任何可能阻塞的调用。**凡是需要睡眠、分配、进入 VFS
  或下发 virtio 命令的操作（`drm_present_buffer_at()`、present 用的
  `ops->flush()`、`vmo_release()`、`ops->resource_unref()`、`memfd_*`、
  `vfs_*`）都在释放锁之后执行。
- 因此拆函数的 teardown 是两段的：`drm_gem_detach_locked()` 在锁内把对象
  移出表，`drm_gem_drop_storage()` 在锁外释放 VMO 与 host resource。
- 跨锁的长操作由 **pin** 兜底：`drm_gem_pin()` 使 `pins++`，让对象在调用者
  放下锁期间不被回收，`drm_gem_unpin()` 归还。`pins` 不是 owner 计数：
  它不会让 buffer 活过最后一个持有者，只保证操作中途不被拆掉。
- **不得在 `g_drm_store.lock` 内调用 VFS。**`vfs_get_file_ref()` 可以一路
  走到 file close op，而 `file_close_prepare()` 是在持有 `g_file_lock` 的
  情况下调用它的。所以 fd→对象、对象→fd 的解析都在锁外完成，锁内只比较
  解析出来的 `vfile.identity` 数值。
- `g_drm_store.lock` 与 `g_vblank.lock` **不得同时持有**。`drm_close()` 目前
  只取 `g_vblank.lock`：它在锁下清掉可能指向本 context 的 pending flip，然后
  释放锁，再做 host 侧的 `ctx_destroy`。这条规则在它**将来**需要清理表项时才
  变成硬约束——那时必须先放掉 `g_vblank.lock` 再取 `g_drm_store.lock`。
- `g_vblank.lock`（既有）保护单槽 pending flip 与 per-file 事件 FIFO。

### VirtIO-GPU transport

`inst->command_lock`（`virtio_gpu.c`）保护共享请求/响应暂存区
（`big_req`/`big_resp`）、descriptor 表、avail/used ring 索引。四个提交
循环都必须在它之下运行。

关键规则：`inst->big_req` 是**每实例一块**共享缓冲，四条路径都会增长、
`kfree` 并重填它。因此 ATTACH_BACKING 与 SUBMIT_3D 的请求体必须在锁内
构建，descriptor 里 `va_to_pa(inst->big_req)` 也必须在（可能的）重新分配
之后计算，否则拿到的是刚被释放的指针。DRM 侧一次 EXECBUFFER 会为每个
buffer 各调一次 ATTACH_BACKING，所以这条路径天然并发。

局部顺序：`inst->command_lock` 是最内层锁。它之下不得调用 VFS、MM 或任何
会睡眠的路径；completion 轮询在锁内 park（`inst->waiters` + Park/Wake），
wake queue 的 flush 在释放锁之后。

### VirtIO input

每实例 `virtio_input_inst_t.lock` 保护 event virtqueue、用户事件 ring 和 waiter，没有局部顺序。IRQ/poll 路径在锁内 drain 有界队列并 collect 一个带token 的 waiter，释放实例锁后 flush wake queue；阻塞 read 使用 Park/Wake协议并在调度前释放锁。

### virtio-console

单实例 `g_vport.lock` 保护字节环、控制台镜像缓冲（`echo`/`echo_len`/`echo_pos`）、rx/tx 两个 `last_used` 游标和计数器。它**没有局部顺序**，因为镜像投递按下面的写法完全在锁外：

- `vport_rx_pump()`（IRQ top-half 与 read/poll 共用）在锁内排空 used ring、推进字节环、重填 avail ring；notify 和控制台镜像都在**解锁之后**做。
- 镜像不能简单地在锁内调 `uart_receive_char()`：那是 UART 的 `rx_lock`（另一个设备私有锁），在本文档里没有记录的嵌套顺序就是 bug；IRQ top-half 也不能用 4 KiB 的栈缓冲。`vport_console_flush()` 因此每轮在锁内只拷 `VPORT_ECHO_BATCH`（64）字节到栈上，释放锁后再逐字节喂给 `uart_receive_char()`，两把锁从不同时持有。
- 发送是同步的：单描述符在飞，`write()` 先在锁内填缓冲与 avail，**释放锁之后**才有界等待 used 游标前进（2 s），超时计 `tx_timeouts` 返回 `-ETIMEDOUT`。等待硬件从不发生在自旋锁内。

### virtio-rng

单实例 `g_vrng.lock` 保护字节环、每槽 `slot_posted[]` 记账、`last_used` 游标、`entropy_bytes`/`read_timeouts` 与 `valid`。它**没有局部顺序**，因为驱动不在锁内调用任何别的锁持有者：

- `vrng_pump()`（IRQ top-half 与 read/poll 共用）在锁内排空 used ring、把字节推进字节环、清 `slot_posted[]`；`dma_sync_for_cpu()` 与 notify 都不在它里面。
- `vrng_post_free()` 按 descriptor → `wmb()` → avail entry → `wmb()` → notify 排序，notify 在**解锁之后**发出。
- read 的有界等待（500 ms `proc_yield()` 循环）在**锁外**进行，每轮只短暂取锁排空。这条是硬约束：等待设备应答属于"等待硬件"，绝不能发生在自旋锁内。
- 熵池投递在锁外。`random_reseed()`（`kernel/core/random.c`）自己取 `rng_lock`，并且经 `arch_entropy_sample()` 触及 `proc_current()` 与 `frame_free_count()`；调用点因此放在 read 路径（进程上下文）而不是 ISR。若将来要让 ISR 喂池，必须先把 `arch_entropy_sample()` 改成中断安全，否则就是在一个中断里读调度器状态。

### xHCI

`xhci_controller_t.lock` 现在是真实生效的 controller 锁：全部 `usb_hcd_ops_t` 回调、`xhci_irq_handler()` 和 `xhci_remove()` 都取它，它保护 command/event ring、endpoint 链、事件环游标、`xfer_busy` 与 `irq_events`/`poll_events`。

规范上「spinlock 下不得等待硬件」不能因为缺锁而绕开，所以等待被移出锁外，用一个所有权标志串行化：

- `xhci_xfer_acquire()` 在锁下自旋取 `xfer_busy`；**持有者取到标志时同时发布 `xfer_deadline`（`XHCI_XFER_HOLD_BUDGET_MS`），自旋方读这个 tick 截止时间而不是数自己的轮数**，过期返回 `-EBUSY`。这是拒绝而不是排队：USB storage 的 `msc_command()` 从 class lock 下调用进来，不能在这里睡眠；而一轮自旋的耗时随机器而变，固定轮数等于在别人的临界区里给出无界时间。两侧共用同一个预算是结构性的：自旋方读到的截止时间不可能早于持有者手上的那个。
- `xhci_drain_events()`/`xhci_wait_event()` 只在锁下取 TRB、分类，更新 `ERDP`；**completion 回调成批在锁外执行**（`xhci_run_completions()` 用同一个 `*flags` 释放再取回），因为 HID completion 会重提 URB，重提直接回到本函数的同一个非递归锁，锁内回调即死锁。
- `xhci_wait_event()` **每轮尝试各取一次锁，取到 TRB 或确认 ring 为空后立刻释放**，锁（含本地中断）绝不跨等待持有：整个 `XHCI_WAIT_LOOPS` 预算一直持锁会让本 CPU 丢掉定时器 tick、其它设备中断和 IPI 服务，`-smp 1` 下就是整机。
- `xhci_irq_handler()` 在 `xfer_busy` 置位时**只 ack `INTR0.IP` 不消费 ring**：只有同步传输的持有者能把自己的 TRB 匹配回请求，抢走它会让控制传输拿到错误的完成；同时电平触发的 INTx 在有界忙等期间会反复重入，只 ack 是让它退化的唯一办法。

因此 HCD **不在持 `xhci->lock` 时回调 class**，class 可以安全地在自己的锁下走到 `xhci->lock`：单向顺序 `class lock -> xhci->lock`。

IRQ handler 内不打日志（它可能在任意上下文抢占 console）：`irq_events`/`poll_events` 由 process-context 的 `xhci_op_poll()` 快照后打印成 `[XHCI] completions: irq=N poll=M`。

已知限制：门禁只跑单核，未做 SMP 压力验证；class remove 与已排队 URB 的 completion 之间仍有窗口。

### USB HID

每接口 `usb_hid_dev_t.lock` 保护 event ring、previous report 和 pending URB 的完成/重提交流程；`usb_hid_complete()` 现在自己取该锁，因此不论 completion 来自 IRQ handler、`usb_core_poll()` 还是别的 endpoint 的 event wait，锁覆盖都是一致的。

局部顺序记为 **`h->lock -> xhci->lock`（单向）**：解析 report 在 `h->lock` 下，重提 URB 走 `usb_submit_interrupt()` 进入 HCD。HCD 不回调 class，所以反向不存在。

配套的两条写法也是规范的一部分：

- `usb_hid_read()`/`usb_hid_poll()` 在取 `h->lock` **之前**调用 `hcd->ops->poll()`，即 `xhci->lock -> h->lock` 这个顺序被显式避免；把 HCD poll 放在 class 锁外，代价是热插拔与读之间的串行化少了一层，但换来锁序单向。
- `usb_hid_remove()` 在释放前清 `urb.complete`/`urb.ctx`，HCD 侧 `xhci_collect_interrupt()` 见到 `complete == NULL` 直接跳过、不碰 `urb->buf`。这**缩小但不消除**该窗口：已经读到指针的 completion 仍可能跑完。

### USB storage

每实例 `usb_storage_dev_t.lock` 保护 BOT tag、CBW/CSW 和共享 4 KiB data buffer。局部顺序 **`st->lock -> xhci->lock`（单向）**。

已知违规未变：`msc_command()` 从 CBW、data 到 CSW 全程持有 `spin_lock_irqsave`，而 xHCI bulk submit 会同步忙等 transfer event。这个实现与本文“spinlock 下不得等待硬件完成”的规范相冲突，是现存限制，不是批准的新例外。xHCI 侧的 `xfer_busy` 有界自旋把「两个同步传输同时抢一个 controller」从静默数据竞争降级为一次 `-EBUSY`，但没有让 storage 本身合规。并发或超时路径修改前应改为 sleepable mutex，或只在 spinlock 下发布/回收并在锁外等待；在此之前不要把 USB storage 当作锁设计范例。

## 跨驱动规则

1. **禁止反向顺序。** 如果必须在持有驱动锁时获取全局锁，需要在本文档中把它记录为局部顺序。未记录的嵌套就是 bug。
2. **禁止在 spinlock 下阻塞。** 任何可能阻塞的路径都必须先释放所有 spinlock。
3. **除非已记录，否则禁止在驱动锁下执行 VFS 和分配。** 唯一已记录的分配例外是：
   - `PTY` 在 `g_pty_alloc_lock` 下执行 `kmalloc`，并在 per-pair lock 下执行最终 `kfree`。驱动完成路径若需要唤醒任务，只能在驱动锁内 collect，在解锁后 flush。 USB storage 的锁内同步硬件等待和 USB HID completion 的锁覆盖缺口是已知不符合项，不属于允许例外。
4. **新的设备锁** 必须符合全局顺序（`driver registry/IRQ locks -> device-private locks`），或在使用前向本文档增加局部顺序条目。

> 不要在设备锁下调用 `kmalloc`、VFS 或 scheduler；不要在 spinlock 里轮询硬件直到超时；不要临时发明"先拿设备锁，再拿 proc_lock"这种嵌套。这些都会在 `make check-concurrency-foundation` 或 SMP smoke 测试里变成死锁或数据竞争。新增锁顺序前请先跑过 [测试门禁](../../testing-gates.md)。

## 参考

- `kernel/include/core/lock.h`：全局锁顺序和 spinlock 原语。
- `kernel/drivers/block/virtio_blk.c`：`inst->lock` 实现。
- `kernel/drivers/net/virtio_net.c`：`net->lock` 与 `g_lwip_lock` 交互。
- `kernel/drivers/net/e1000.c`：`nic->lock`，中断排空与发送路径共用。
- `kernel/drivers/net/rtl8139.c`：`nic->lock`，中断处理函数与 send/recv/poll 共用。
- `kernel/drivers/char/uart.c`：`rx_lock` 和 Ctrl-C signal 路径。
- `kernel/drivers/char/virtio_console.c`：`g_vport.lock`，锁外 notify、锁外控制台镜像与锁外的同步发送等待。
- `kernel/drivers/char/pty.c`：`g_pty_alloc_lock` 和 per-pair `lock`。
- `kernel/drivers/block/loop.c`：per-device loop lock。
- `kernel/drivers/block/dw_sdio.c`：`g_sdio.lock`（mutex）串行化轮询与中断两条传输路径，`g_sdio.irq_lock` 保护中断 latch。
- `kernel/drivers/net/starfive_gmac.c`：per-instance lock 串行化 descriptor ring。
- `kernel/drivers/net/ls2k_gmac.c`：per-instance lock 串行化 descriptor ring。
- `kernel/platform/visionfive2/board.c`、`kernel/platform/ls2k1000/board.c`：物理板适配，见 [物理开发板移植](../../platforms/physical-boards.md)。
