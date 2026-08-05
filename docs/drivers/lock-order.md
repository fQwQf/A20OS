# 驱动锁顺序契约

本文是 A20OS 内核驱动私有锁的权威锁顺序契约。它补充 `kernel/include/core/lock.h` 中的全局契约，并记录每个驱动的例外情况和局部顺序。

## 范围

覆盖的驱动：

| 驱动 | 文件 | 私有锁 |
|--------|------|--------------|
| virtio-blk | `kernel/drivers/block/virtio_blk.c` | `inst->lock`（spinlock） |
| virtio-net | `kernel/drivers/net/virtio_net.c` | `net->lock`（spinlock） |
| UART | `kernel/drivers/char/uart.c` | `rx_lock`（spinlock） |
| PTY | `kernel/drivers/char/pty.c` | `g_pty_alloc_lock`、每个 pair 的 `lock`（spinlock） |
| loop | `kernel/drivers/block/loop.c` | `g_loop[i].lock`（spinlock） |
| DW SDIO | `kernel/drivers/block/dw_sdio.c` | 无 |
| StarFive GMAC | `kernel/drivers/net/starfive_gmac.c` | 无 |
| Loongson-2K GMAC | `kernel/drivers/net/ls2k_gmac.c` | 无 |
| AHCI | `kernel/drivers/block/ahci.c` | `port->lock`（mutex） |
| VirtIO-SCSI | `kernel/drivers/block/virtio_scsi.c` | `dev->lock`（mutex） |
| NVMe | `kernel/drivers/block/nvme.c` | `ctrl->io_lock`（mutex） |
| E1000 | `kernel/drivers/net/e1000.c` | `nic->lock`（spinlock） |
| VMSVGA/SVGAv3 | `kernel/drivers/gpu/vmsvga.c` | `svga->lock`（spinlock） |
| VirtIO-GPU | `kernel/drivers/gpu/virtio_gpu.c` | `inst->command_lock`（mutex） |
| VirtIO-SND | `kernel/drivers/audio/virtio_snd.c` | `snd->lock`（mutex） |
| VirtIO input | `kernel/drivers/input/virtio_input.c` | `inst->lock`（spinlock） |
| xHCI HID | `kernel/drivers/input/xhci_hid.c` | `xhci->lock`（spinlock） |

## 全局顺序摘要

`kernel/include/core/lock.h` 中的全局顺序为：

```text
cg_node.lock -> proc_lock -> runq_lock -> pfa.lockproc_lock -> runq_lockproc_lock -> files_struct.lock -> VFS global-file/vnode locksproc_lock -> mm_struct.lockproc_lock -> a20_handle_table.lockdriver registry/IRQ locks -> device-private locksg_lwip_lock -> virtio-net nonblocking send/recv paths only
```

对驱动而言，这意味着：

- 设备私有锁永远是最内层锁。
- 持有设备私有锁时，绝不能获取 `proc_lock`、`files_struct.lock`、VFS 锁、`mm_struct.lock`、`a20_handle_table.lock` 或 `runq_lock`，除非下文的具体局部顺序记录了该例外。
- 持有 spinlock 时绝不能阻塞、睡眠或调入调度器。
- 持有设备锁或 lwIP 锁时，绝不能调用内存分配、VFS 或 scheduler 路径，除非 callee 明确记录为非阻塞。

## 各驱动契约

### virtio-blk

**锁：** `virtio_blk_inst_t.lock`（`inst->lock`），每个实例一个。

**保护内容：**

- 请求 slot（`inst->req[]`）
- descriptor/available/used ring 状态
- `inst->in_flight`
- `inst->status[]`、`inst->req_hdr[]`
- `blk->last_used`、`blk->desc_idx`

**局部顺序：** 无。`inst->lock` 从不与其他内核锁嵌套。

**规则：**

- 提交、完成轮询和请求分配都在 `inst->lock` 下运行。
- 完成路径在 `inst->lock` 下通过 `wait_queue_collect_all()` 把 task 引用和
  `wait_seq` 转移到局部 wake queue；释放 `inst->lock` 后才调用`proc_wake_q_flush()` 进入 scheduler。
- 当没有可用 slot 时，`virtio_blk_rw()` 在 yield CPU 前释放锁。
- 不要在 `inst->lock` 下调用 VFS、`kmalloc` 或阻塞式 scheduler 函数。

### virtio-net

**锁：** `virtio_net_inst_t.lock`（`net->lock`），每个实例一个。

**保护内容：**

- TX/RX descriptor ring
- `tx_busy[]`、`rx_buf[]`、`tx_buf[]`
- 队列索引（`last_used`、`avail->idx`）
- 统计信息（`rx_packets`、`tx_packets`、`rx_drops`、`tx_drops`）

**局部顺序：** `g_lwip_lock -> net->lock`。

- lwIP raw-API send 路径在持有 `g_lwip_lock` 时调用。
- `virtio_net_send()` 且 `nonblock == 1` 是唯一可以在 `g_lwip_lock` 下运行的 virtio-net 入口点。
- nonblock 路径绝不睡眠等待 TX 完成。它提交 descriptor 后立即返回。
- 阻塞 send/recv 路径（`nonblock == 0`，或 `virtio_net_recv()`）不得在 `g_lwip_lock` 下调用，因为它们可能睡眠。

**规则：**

- 绝不跨 scheduler wait 持有 `net->lock`。
- 持有 `net->lock` 时绝不调用 lwIP 函数（顺序是 lwIP 外层、net 内层）。
- `virtio_net_poll_all()` 和 `virtio_net_class_poll()` 只获取 `net->lock`。

### UART

**锁：** `rx_lock`，单个全局 spinlock。

**保护内容：**

- 接收环形缓冲区（`rx_buffer`、`rx_head`、`rx_tail`）
- `rx_waiter`
- `tty_foreground_pgid`

**局部顺序：** 无。`rx_lock` 不与 `proc_lock` 嵌套。

- 普通 RX 处理只在 `rx_lock` 下触碰环形缓冲区和前台 PGID。
- RX 唤醒在 `rx_lock` 下 collect wait entry，释放锁后 flush wake queue。
- Ctrl-C 路径不持有 `rx_lock`；task dump、带引用 PID 查询和信号发送各自在
  驱动锁外获取所需的进程锁。

**规则：**

- 所有 UART 路径在持有 `rx_lock` 时不得获取额外锁。
- `uart_getc()` 使用 prepare → 锁内重查/link → unlock → commit 的
  Park/Wake 协议，并在阻塞当前任务前释放 `rx_lock`。

### PTY

**锁：**

- `g_pty_alloc_lock`：用于 pair 分配和初始设置的全局 spinlock。
- `g_ptys[idx].lock`：用于某个 pair 所有运行时操作的 per-pair spinlock。

**`g_pty_alloc_lock` 保护内容：**

- 分配期间的 `in_use` 标志
- `m2s_buf` / `s2m_buf` 分配和初始字段设置

**per-pair `lock` 保护内容：**

- 环形缓冲区（`m2s_*`、`s2m_*`）
- `master_refs`、`slave_refs`
- `locked`、`master_nonblock`、`slave_nonblock`
- `master_waiting`、`slave_waiting` 和 termios 状态
- 窗口大小（`ws_row`、`ws_col`）

**局部顺序：** `g_pty_alloc_lock` 与 per-pair lock 从不嵌套；阻塞 read 入队时使用 `per-pair lock -> wait-queue lock`。

- `pty_alloc()` 获取 `g_pty_alloc_lock`，用 `kmalloc` 分配缓冲区，初始化 pair，然后释放 `g_pty_alloc_lock`。分配期间不持有 per-pair lock。
- 所有 read/write/ioctl 路径只获取 per-pair lock。
- master/slave close 路径获取 per-pair lock 来更新各自的引用计数；只有两端引用和等待者都清零后才释放缓冲区并清除 `in_use`。
- 阻塞 read 在持有 per-pair lock 时把任务加入对应 wait queue，再释放 pair lock 并调度；write 和对端 close 会唤醒该队列。

**规则：**

- 不要跨 VFS 操作持有 per-pair lock。等待队列操作只按 `per-pair lock -> wait-queue lock` 的局部顺序短暂嵌套。
- `g_pty_alloc_lock` 在 `kmalloc` 期间保持持有。这是已知例外；新的代码应尽量避免在该锁下分配。

### loop

**锁：** `g_loop[i].lock`，每个 loop device 一个。

**保护内容：**

- `in_use`
- `backing_vf`
- `backing_size`

**局部顺序：** 无。

- `loop_set_fd()` 在获取 loop lock 之前先从 VFS 获得 `vfile_t` 引用和大小。
- `loop_clr_fd()` 在 loop lock 下清除状态，然后释放锁，再释放 `vfile_t` 引用。
- `loop_dev_read()` 和 `loop_dev_write()` 在 loop lock 下复制 `backing_vf` 和 `backing_size`，释放锁后再无锁调用 backing file 的 `lseek`/`read`/`write` 操作。

**规则：**

- loop lock 从不跨 backing-file VFS 操作持有。
- loop lock 下不发生内存分配。

### DW SDIO

**锁：** 无。

- 该驱动使用单个全局 `sdio_priv_t` 实例（`g_sdio`）。
- 所有命令和数据传输都是同步 busy-poll。
- 当前实现一次只允许一个 in-flight 请求，且不支持并发调用者，因此不需要私有 spinlock。

**规则：**

- 未来的并发版本或 IRQ 驱动版本必须增加私有锁，并在本文档中记录。
- 在此之前，调用者通过单实例进行串行化。

### StarFive GMAC

**锁：** 无。

- 该驱动使用单个全局 `gmac_priv_t` 实例（`g_gmac`）。
- `starfive_gmac_send()` 和 `starfive_gmac_recv()` 是寄存器轮询路径，会检查 descriptor ownership bit。
- 当前实现没有用私有 spinlock 保护 descriptor ring。

**规则：**

- 未来的 IRQ 驱动版本或 SMP-safe 版本必须增加私有锁，并在本文档中记录。
- 今天的并发 send/recv 调用存在竞争；调用者必须在外部串行化。

### Loongson-2K GMAC

**锁：** 无。

- 该驱动使用单个全局 `ls2k_gmac_priv_t` 实例（`g_ls2k_gmac`）。
- `ls2k_gmac_send()` 和 `ls2k_gmac_recv()` 是寄存器轮询路径，会检查 descriptor ownership bit。
- 当前实现没有用私有 spinlock 保护 descriptor ring。

**规则：**

- 未来的 IRQ 驱动版本或 SMP-safe 版本必须增加私有锁，并在本文档中记录。
- 今天的并发 send/recv 调用存在竞争；调用者必须在外部串行化。

### AHCI

**锁：** `ahci_port_t.lock`（`port->lock`），可睡眠 mutex，串行化单端口 command slot、command table 和共享 transfer buffer。

**局部顺序：** 无。类 read/write 持有该 mutex 完成分块 I/O；命令完成等待在持锁状态下按标准完成模型 park 在 `port->waiters` 上。IRQ top-half 不获取该 mutex——它只记录 `port->last_is`、清 PxIS 并 collect waiters（wait queue 内部自锁）。

**规则：**

- IRQ handler 不得在 IRQ 上下文获取 `port->lock`（mutex 不可在 IRQ 中使用）；这正是完成状态经 `last_is` 移交而不是由 handler 推进 ring 的原因。
- 持 mutex park 是允许的，因为竞争者只会阻塞在同一 mutex 上，而唤醒来自不持锁的 top-half。

### VirtIO-SCSI

**锁：** `virtio_scsi_dev_t.lock`（`dev->lock`），可睡眠 mutex，保护 request queue descriptor、avail/used index、共享 request/response 和单命令 buffer。

**局部顺序：** 无。当前每控制器只有一个同步 in-flight 命令；命令等待在持锁状态下 park 在 `dev->waiters` 上，IRQ top-half 只 collect waiters，不获取该 mutex。

### NVMe

**锁：** `nvme_controller_t.io_lock`（`ctrl->io_lock`），可睡眠 mutex，串行化 I/O queue 的提交与完成消费。

**局部顺序：** 无。提交者独占 `cq_head`/`cq_db`；等待在持锁状态下 park 在 `q->waiters` 上（admin queue 仅在 probe 期使用，保持有界轮询）。IRQ top-half 只 collect admin/io 两个 wait queue，不触碰 queue 状态，也不获取 `io_lock`。

**规则：**

- `ctrl->failed` 用原子操作读写，允许 handler 外的路径在不持锁时观察设备致命状态。

### VirtIO-GPU

**锁：** `virtio_gpu_inst_t.command_lock`（`inst->command_lock`），可睡眠 mutex，串行化 controlq 的单条 in-flight 命令链。

**局部顺序：** 无。flush 等待在持锁状态下 park 在 `inst->waiters` 上（boot 早期无 `proc_current()` 或 IRQ 未注册时保持有界 poll/yield）。IRQ top-half 只 ack ISR 并 collect waiters，不获取该 mutex。

**已知限制：** controlq 单命令链意味着显示 flush 是全局串行的；未来多队列/异步实现必须改为 per-request 状态与 completion，禁止退回自旋锁内长等待或栈 DMA。

### VirtIO-SND

**锁：** `virtio_snd_dev_t.lock`（`snd->lock`），可睡眠 mutex，串行化 PCM 命令、staging 和 stream 状态机。

**局部顺序：** 无。所有完成等待经 `virtio_snd_wait_step()`：有 IRQ 且存在当前任务时 park 在 `snd->waiters` 上（20 ms 有界块保持 generation 中断响应），否则 yield/relax。IRQ top-half 只 ack 并 collect waiters，不获取 `snd->lock`——这保证了持锁睡眠的 issuer 与 handler 之间没有锁序环。

### E1000

**锁：** `e1000_device_t.lock`（`nic->lock`），保护 RX/TX descriptor、buffer 和 `rx_next/tx_next`。

**局部顺序：** `g_lwip_lock -> nic->lock`。send/recv/poll 可由 lwIP 在外层锁下调用，驱动锁下不得回调 lwIP、分配或睡眠。

### VMSVGA/SVGAv3

**锁：** `vmsvga_device_t.lock`（`svga->lock`），保护 command buffer header、command submission 和 update 序列。

**局部顺序：** 无。`flush` 在锁内提交并轮询短 command completion；不得在此锁下执行 framebuffer 映射、VFS 或用户 copy。

### VirtIO input

**锁：** 每实例 `virtio_input_inst_t.lock`，保护 event virtqueue、用户事件 ring 和 waiter。

**局部顺序：** 无。IRQ/poll 路径在锁内 drain 有界队列并 collect 一个带token 的 waiter，释放实例锁后 flush wake queue；阻塞 read 使用 Park/Wake协议并在调度前释放锁。

### xHCI HID

**锁：** `xhci_controller_t.lock`，保护 command/event/endpoint ring、HID report 状态和聚合 input ring。

**局部顺序：** 无。当前 class read/poll 在锁内推进轮询；不得从该路径调用 VFS、分配或调度。

## 跨驱动规则

1. **禁止反向顺序。** 如果必须在持有驱动锁时获取全局锁，需要在本文档中把它记录为局部顺序。未记录的嵌套就是 bug。
2. **禁止在 spinlock 下阻塞。** 任何可能阻塞的路径都必须先释放所有 spinlock。需要跨命令等待保持串行化时，使用可睡眠 `mutex_t` 而不是 spinlock，并按 [标准完成模型](runtime-contracts.md#标准完成模型irq-hybrid) park。
3. **除非已记录，否则禁止在驱动锁下执行 VFS 和分配。** 唯一已记录的例外是：
   - `PTY` 分配在 `g_pty_alloc_lock` 下执行 `kmalloc`。
   驱动完成路径若需要唤醒任务，只能在驱动锁内 collect，在解锁后 flush。
4. **IRQ top-half 不获取实例 mutex。** IRQ 上下文只能使用内部自锁的 `wait_queue_t` 移交唤醒；handler 不得推进提交者拥有的 ring 状态（AHCI 的 `last_is` 移交是已记录的模式）。这保证了"持 mutex park"不会形成锁序环。
5. **新的设备锁** 必须符合全局顺序（`driver registry/IRQ locks -> device-private locks`），或在使用前向本文档增加局部顺序条目。

> 不要这样做
> 在设备锁下调用 `kmalloc`、VFS 或 scheduler；在 spinlock 里轮询硬件直到超时；临时发明一种“先拿设备锁，再拿 proc_lock”的嵌套。这些都会在 `make check-concurrency-foundation` 或 SMP smoke 测试里变成死锁或数据竞争。新增锁顺序前请先跑过 [测试门禁](../testing/testing-gates.md)。

## 参考

- `kernel/include/core/lock.h`：全局锁顺序和 spinlock 原语。
- `kernel/drivers/block/virtio_blk.c`：`inst->lock` 实现。
- `kernel/drivers/net/virtio_net.c`：`net->lock` 与 `g_lwip_lock` 交互。
- `kernel/drivers/char/uart.c`：`rx_lock` 和 Ctrl-C signal 路径。
- `kernel/drivers/char/pty.c`：`g_pty_alloc_lock` 和 per-pair `lock`。
- `kernel/drivers/block/loop.c`：per-device loop lock。
- `kernel/drivers/block/dw_sdio.c`：无锁 SDIO 实现。
- `kernel/drivers/net/starfive_gmac.c`：无锁 StarFive GMAC 实现。
- `kernel/drivers/net/ls2k_gmac.c`：无锁 Loongson-2K GMAC 实现。
