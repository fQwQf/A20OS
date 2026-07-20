# 驱动锁顺序契约

本文是 A20OS 内核驱动私有锁的权威锁顺序契约。它补充 `kernel/include/core/lock.h` 中的全局契约，并记录每个驱动的例外情况和局部顺序。

## 范围

覆盖的驱动：

| 驱动 | 文件 | 私有锁 |
|--------|------|--------------|
| virtio-blk | `kernel/drivers/block/virtio_blk.c` | `inst->lock` |
| virtio-net | `kernel/drivers/net/virtio_net.c` | `net->lock` |
| UART | `kernel/drivers/char/uart.c` | `rx_lock` |
| PTY | `kernel/drivers/char/pty.c` | `g_pty_alloc_lock`、每个 pair 的 `lock` |
| loop | `kernel/drivers/block/loop.c` | `g_loop[i].lock` |
| DW SDIO | `kernel/drivers/block/dw_sdio.c` | 无 |
| StarFive GMAC | `kernel/drivers/net/starfive_gmac.c` | 无 |
| Loongson-2K GMAC | `kernel/drivers/net/ls2k_gmac.c` | 无 |
| AHCI | `kernel/drivers/block/ahci.c` | `port->lock` |
| VirtIO-SCSI | `kernel/drivers/block/virtio_scsi.c` | `dev->lock` |
| E1000 | `kernel/drivers/net/e1000.c` | `nic->lock` |
| VMSVGA/SVGAv3 | `kernel/drivers/gpu/vmsvga.c` | `svga->lock` |
| VirtIO input | `kernel/drivers/input/virtio_input.c` | `inst->lock` |
| xHCI HID | `kernel/drivers/input/xhci_hid.c` | `xhci->lock` |

## 全局顺序摘要

`kernel/include/core/lock.h` 中的全局顺序为：

```text
cg_node.lock -> proc_lock -> runq_lock -> pfa.lock
proc_lock -> runq_lock
proc_lock -> files_struct.lock -> VFS global-file/vnode locks
proc_lock -> mm_struct.lock
proc_lock -> a20_handle_table.lock
driver registry/IRQ locks -> device-private locks
g_lwip_lock -> virtio-net nonblocking send/recv paths only
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
- 完成路径在持有 `inst->lock` 时调用 `proc_make_ready(req->waiter)`。这是安全的，因为设备锁是最内层锁，而 `proc_make_ready` 只通过自身加锁触碰 runqueue 侧。
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

**局部顺序：** `rx_lock -> proc_lock`。

- 普通 RX 处理只在 `rx_lock` 下触碰环形缓冲区和 `rx_waiter`。
- Ctrl-C 路径（`uart_rx_push()` 处理 `0x03`）持有 `rx_lock`，随后通过 `uart_signal_user_pgid()` / `uart_signal_all_user()` 调用 `proc_find()` / `proc_kill()`。这些函数会获取 `proc_lock`。
- `uart_dump_tasks()` 也会获取 `proc_lock`，并在 Ctrl-C 路径中于 `rx_lock` 下调用。
- 这是唯一记录在案的驱动私有锁嵌套 `proc_lock` 例外。

**规则：**

- 其他所有 UART 路径在持有 `rx_lock` 时不得获取额外锁。
- `uart_getc()` 在阻塞当前任务前释放 `rx_lock`。

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

**锁：** `ahci_port_t.lock`（`port->lock`），保护单端口 command slot、command table 和共享 transfer buffer。

**局部顺序：** 无。类 read/write 获取该锁后串行完成分块 I/O。

**已知限制：** 当前实现会在锁内轮询命令完成，最长可达硬件超时。这不适合作为新驱动范例；IRQ/completion 化时必须只在锁内发布和回收 slot，在无锁状态等待完成。

### VirtIO-SCSI

**锁：** `virtio_scsi_dev_t.lock`（`dev->lock`），保护 request queue descriptor、avail/used index、共享 request/response 和单命令 buffer。

**局部顺序：** 无。当前每控制器只有一个同步 in-flight 命令。

**已知限制：** VirtIO GPU controlq 当前用实例 mutex 串行化，在 mutex 内轮询完成但不关闭中断。请求/响应位于实例 DMA staging。未来多队列/异步实现必须改为 per-request 状态与 completion；禁止退回自旋锁内长等待或栈 DMA。

### E1000

**锁：** `e1000_device_t.lock`（`nic->lock`），保护 RX/TX descriptor、buffer 和 `rx_next/tx_next`。

**局部顺序：** `g_lwip_lock -> nic->lock`。send/recv/poll 可由 lwIP 在外层锁下调用，驱动锁下不得回调 lwIP、分配或睡眠。

### VMSVGA/SVGAv3

**锁：** `vmsvga_device_t.lock`（`svga->lock`），保护 command buffer header、command submission 和 update 序列。

**局部顺序：** 无。`flush` 在锁内提交并轮询短 command completion；不得在此锁下执行 framebuffer 映射、VFS 或用户 copy。

### VirtIO input

**锁：** 每实例 `virtio_input_inst_t.lock`，保护 event virtqueue、用户事件 ring 和 waiter。

**局部顺序：** 无。IRQ/poll 路径在锁内 drain 有界队列并可调用 `proc_make_ready`；阻塞 read 在调度前释放锁。

### xHCI HID

**锁：** `xhci_controller_t.lock`，保护 command/event/endpoint ring、HID report 状态和聚合 input ring。

**局部顺序：** 无。当前 class read/poll 在锁内推进轮询；不得从该路径调用 VFS、分配或调度。

## 跨驱动规则

1. **禁止反向顺序。** 如果必须在持有驱动锁时获取全局锁，需要在本文档中把它记录为局部顺序。未记录的嵌套就是 bug。
2. **禁止在 spinlock 下阻塞。** 任何可能阻塞的路径都必须先释放所有 spinlock。
3. **除非已记录，否则禁止在驱动锁下执行 VFS 和分配。** 唯一已记录的例外是：
   - `virtio-blk` 完成路径调用 `proc_make_ready`（调度通知，不是 VFS）。
   - `UART` Ctrl-C 路径在 `rx_lock` 下嵌套 `proc_lock`。
   - `PTY` 分配在 `g_pty_alloc_lock` 下执行 `kmalloc`。
4. **新的设备锁** 必须符合全局顺序（`driver registry/IRQ locks -> device-private locks`），或在使用前向本文档增加局部顺序条目。

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
