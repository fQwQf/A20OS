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
| AHCI | `kernel/drivers/block/ahci.c` | sleepable `port->lock` mutex |
| VirtIO-SCSI | `kernel/drivers/block/virtio_scsi.c` | sleepable `dev->lock` mutex |
| E1000 | `kernel/drivers/net/e1000.c` | `nic->lock` |
| VMSVGA/SVGAv3 | `kernel/drivers/gpu/vmsvga.c` | `svga->lock` |
| VirtIO input | `kernel/drvmod/examples/vinput.c (inst->lock 在模块内)` | `inst->lock` |
| xHCI | `kernel/drivers/usb/host/xhci.c`（generic 模块 `xhci.a20drv`） | `xhci->lock` 已初始化但当前未获取；不是已生效的同步保证 |
| USB HID | `kernel/drivers/usb/class/usb_hid.c` | 每接口 `h->lock`；completion 路径当前未一致获取 |
| USB storage | `kernel/drivers/usb/class/usb_storage.c` | 每实例 `st->lock`；当前跨同步 bulk wait 持有 |

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
- 完成路径在 `inst->lock` 下通过 `wait_queue_collect_all()` 把 task 引用和 `wait_seq` 转移到局部 wake queue；释放 `inst->lock` 后才调用 `proc_wake_q_flush()` 进入 scheduler。
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
- Ctrl-C 路径不持有 `rx_lock`；task dump、带引用 PID 查询和信号发送各自在驱动锁外获取所需的进程锁。

**规则：**

- 所有 UART 路径在持有 `rx_lock` 时不得获取额外锁。
- `uart_getc()` 使用 prepare → 锁内重查/link → unlock → commit 的 Park/Wake 协议，并在阻塞当前任务前释放 `rx_lock`。

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
- `g_pty_alloc_lock` 在 `kmalloc` 期间保持持有，`pty_maybe_free_locked()` 也在 per-pair lock 下执行 `kfree`。两者都是现存例外；新的代码不得复制这种模式。

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

**锁：** `g_sdio.lock`（spinlock），保护命令/数据寄存器序列与 `g_sdio` 的 `ready/rca/sectors`。

- 该驱动使用单个全局 `sdio_priv_t` 实例（`g_sdio`）。
- 所有命令和数据传输都是同步 busy-poll；`sdio_xfer_block()` 在 spinlock 下执行一次阻塞传输。
- 短轮询（`sdio_wait_cmd`/`sdio_wait_data_idle`）有超时上限，不会无限自旋。

**规则：**

- 锁内只做寄存器轮询与数据搬运，不分配、不调用 VFS、不睡眠。
- IRQ 驱动的完成路径若未来加入，必须复用同一锁并在本文档记录单向顺序。

### StarFive GMAC

**锁：** 每实例 `starfive_gmac_priv_t.lock`（spinlock），保护 TX/RX descriptor ring 与 `tx_busy/rx_busy`。

- 实例池 `g_gmac_insts[]`（最多 4 个）按 MMIO 基址查找，probe 时分配。
- `starfive_gmac_send()`、`starfive_gmac_recv()`、`starfive_gmac_poll()` 在锁内完成寄存器轮询路径并检查 descriptor ownership bit。
- 锁内不做分配、VFS 或回调网络栈；`mdelay()` 只出现在 init/PHY 阶段（锁外）。

**规则：**

- IRQ 驱动的完成路径必须复用同一锁并在此记录顺序。
- 多实例已支持，但数据面仍是轮询，未接 IRQ。

### Loongson-2K GMAC

**锁：** 每实例 `ls2k_gmac_priv_t.lock`（spinlock），保护 TX/RX descriptor ring 与 `tx_busy/rx_busy`。

- 实例池 `g_ls2k_gmac_insts[]`（最多 4 个）按 MMIO 基址查找，probe 时分配。
- `ls2k_gmac_send()`、`ls2k_gmac_recv()`、`ls2k_gmac_poll()` 在锁内完成寄存器轮询路径并检查 descriptor ownership bit。
- 锁内不做分配、VFS 或回调网络栈。

**规则：**

- IRQ 驱动的完成路径必须复用同一锁并在此记录顺序。
- 数据面仍是轮询，未接 IRQ。

### AHCI

**锁：** `ahci_port_t.lock`（`port->lock`）是 sleepable mutex，保护单端口 command slot、command table 和共享 transfer buffer。

**局部顺序：** 无。类 read/write 获取该锁后串行完成分块 I/O。

**等待模型：** mutex 串行化整个命令。提交后先做有界短轮询；有 IRQ 时在仍持有 sleepable mutex 的情况下 park 到 wait queue，top-half 不获取 mutex，只记录状态并唤醒；无 IRQ 时回退有界轮询。这里允许睡眠是因为它不是 spinlock，但仍限制每端口一个 in-flight 命令。

### VirtIO-SCSI

**锁：** `virtio_scsi_dev_t.lock`（`dev->lock`），保护 request queue descriptor、avail/used index、共享 request/response 和单命令 buffer。

**局部顺序：** 无。`dev->lock` 是 sleepable mutex，当前每控制器只有一个同步 in-flight 命令。

**已知限制：** 每控制器当前只有一个同步 in-flight 命令，且使用混合完成窗口——先在锁内短时自旋轮询（`VIRTIO_SCSI_HYBRID_PRE_POLL_US`），随后 park 等待（IRQ 注册失败时回退纯轮询）。睡眠锁 `dev->lock` 不被 IRQ top-half 持有，因此轮询窗口不会阻塞中断路径。未来多队列实现必须改为 per-request 状态与 completion。

### E1000

**锁：** `e1000_device_t.lock`（`nic->lock`），保护 RX/TX descriptor、buffer 和 `rx_next/tx_next`。

**局部顺序：** `g_lwip_lock -> nic->lock`。send/recv/poll 可由 lwIP 在外层锁下调用，驱动锁下不得回调 lwIP、分配或睡眠。

### VMSVGA/SVGAv3

**锁：** `vmsvga_device_t.lock`（`svga->lock`），保护 command buffer header、command submission 和 update 序列。

**局部顺序：** 无。`flush` 在锁内提交并轮询短 command completion；不得在此锁下执行 framebuffer 映射、VFS 或用户 copy。

### VirtIO input

**锁：** 每实例 `virtio_input_inst_t.lock`，保护 event virtqueue、用户事件 ring 和 waiter。

**局部顺序：** 无。IRQ/poll 路径在锁内 drain 有界队列并 collect 一个带token 的 waiter，释放实例锁后 flush wake queue；阻塞 read 使用 Park/Wake协议并在调度前释放锁。

### xHCI

**锁状态：** `xhci_controller_t.lock` 在 probe 中初始化，但当前 `xhci.c` 没有获取它。不能把该字段描述成已经保护 command/event/endpoint ring 或端口状态。

当前 HCD 依赖 USB core 的 process-context 轮询和调用方序列化；USB storage/HID 各自的 class lock 不构成 controller 级并发保护。未来若让 URB 提交、hotplug poll 或多个 class 调用真正并发，必须先实现并审查 controller 锁，再记录 class lock 与 HCD lock 的单向顺序，不能把计划中的顺序当成现状。

### USB HID

**锁：** 每接口 `usb_hid_dev_t.lock`，设计上应保护 event ring、previous report 和 pending URB 的完成/重提交流程。

read/poll 在 `h->lock` 下调用 HCD `poll`，但 `usb_hid_complete()` 自身不获取该锁。completion 不只来自这个入口：周期性的 `usb_core_poll()`，以及 xHCI 等待其他 command/bulk event 时顺带处理的 interrupt event，也会直接调用它。因此当前 callback 可能在没有 `h->lock` 时修改 report/event ring 并重提 URB，锁覆盖并不完整。再加上 xHCI controller lock 未生效，多个 poll/wait 调用还可能并发消费同一 event ring。修复时必须统一 completion 串行化，并明确 class lock 与 controller/endpoint lock 的单向顺序；不能把现状描述为 SMP-safe。

### USB storage

**锁：** 每实例 `usb_storage_dev_t.lock`，保护 BOT tag、CBW/CSW 和共享 4 KiB data buffer。

当前 `msc_command()` 从 CBW、data 到 CSW 全程持有 `spin_lock_irqsave`，而 xHCI bulk submit 会同步轮询 transfer event。这个实现与本文“spinlock 下不得等待硬件完成”的规范相冲突，是现存限制，不是批准的新例外。并发或超时路径修改前应改为 sleepable mutex，或只在 spinlock 下发布/回收并在锁外等待；在此之前不要把 USB storage 当作锁设计范例。

## 跨驱动规则

1. **禁止反向顺序。** 如果必须在持有驱动锁时获取全局锁，需要在本文档中把它记录为局部顺序。未记录的嵌套就是 bug。
2. **禁止在 spinlock 下阻塞。** 任何可能阻塞的路径都必须先释放所有 spinlock。
3. **除非已记录，否则禁止在驱动锁下执行 VFS 和分配。** 唯一已记录的分配例外是：
   - `PTY` 在 `g_pty_alloc_lock` 下执行 `kmalloc`，并在 per-pair lock 下执行最终 `kfree`。驱动完成路径若需要唤醒任务，只能在驱动锁内 collect，在解锁后 flush。
   USB storage 的锁内同步硬件等待和 USB HID completion 的锁覆盖缺口是已知不符合项，不属于允许例外。
4. **新的设备锁** 必须符合全局顺序（`driver registry/IRQ locks -> device-private locks`），或在使用前向本文档增加局部顺序条目。

> 不要这样做在设备锁下调用 `kmalloc`、VFS 或 scheduler；在 spinlock 里轮询硬件直到超时；临时发明一种“先拿设备锁，再拿 proc_lock”的嵌套。这些都会在 `make check-concurrency-foundation` 或 SMP smoke 测试里变成死锁或数据竞争。新增锁顺序前请先跑过 [测试门禁](../../testing/testing-gates.md)。

## 参考

- `kernel/include/core/lock.h`：全局锁顺序和 spinlock 原语。
- `kernel/drivers/block/virtio_blk.c`：`inst->lock` 实现。
- `kernel/drivers/net/virtio_net.c`：`net->lock` 与 `g_lwip_lock` 交互。
- `kernel/drivers/char/uart.c`：`rx_lock` 和 Ctrl-C signal 路径。
- `kernel/drivers/char/pty.c`：`g_pty_alloc_lock` 和 per-pair `lock`。
- `kernel/drivers/block/loop.c`：per-device loop lock。
- `kernel/drivers/block/dw_sdio.c`：`g_sdio.lock` 串行化轮询传输。
- `kernel/drivers/net/starfive_gmac.c`：per-instance lock 串行化 descriptor ring。
- `kernel/drivers/net/ls2k_gmac.c`：per-instance lock 串行化 descriptor ring。
- `kernel/platform/visionfive2/board.c`、`kernel/platform/ls2k1000/board.c`：物理板适配，见 [物理开发板移植](../platforms/physical-boards.md)。
