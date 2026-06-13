# Driver Lock-Order Contract

This document is the authoritative lock-order contract for private locks in A20 kernel drivers. It supplements the global contract in `kernel/include/core/lock.h` and is the place where per-driver exceptions and local orders are recorded.

## Scope

Covered drivers:

| Driver | File | Private lock |
|--------|------|--------------|
| virtio-blk | `kernel/drivers/block/virtio_blk.c` | `inst->lock` |
| virtio-net | `kernel/drivers/net/virtio_net.c` | `net->lock` |
| UART | `kernel/drivers/char/uart.c` | `rx_lock` |
| PTY | `kernel/drivers/char/pty.c` | `g_pty_alloc_lock`, per-pair `lock` |
| loop | `kernel/drivers/block/loop.c` | `g_loop[i].lock` |
| DW SDIO | `kernel/drivers/block/dw_sdio.c` | none |
| StarFive GMAC | `kernel/drivers/net/starfive_gmac.c` | none |
| Loongson-2K GMAC | `kernel/drivers/net/ls2k_gmac.c` | none |

## Global order summary

The global order from `kernel/include/core/lock.h` is:

```text
cg_node.lock -> proc_lock -> runq_lock -> pfa.lock
proc_lock -> runq_lock
proc_lock -> files_struct.lock -> VFS global-file/vnode locks
proc_lock -> mm_struct.lock
proc_lock -> a20_handle_table.lock
driver registry/IRQ locks -> device-private locks
g_lwip_lock -> virtio-net nonblocking send/recv paths only
```

For drivers this means:

- Device-private locks are always the innermost locks.
- Never acquire `proc_lock`, `files_struct.lock`, VFS locks, `mm_struct.lock`, `a20_handle_table.lock`, or `runq_lock` while holding a device-private lock, unless a specific local order below documents the exception.
- Never block, sleep, or call into the scheduler while holding a spinlock.
- Never call memory allocation, VFS, or scheduler paths while holding a device or lwIP lock unless the callee is documented nonblocking.

## Per-driver contracts

### virtio-blk

**Lock:** `virtio_blk_inst_t.lock` (`inst->lock`), one per instance.

**What it protects:**

- Request slots (`inst->req[]`)
- Descriptor/available/used ring state
- `inst->in_flight`
- `inst->status[]`, `inst->req_hdr[]`
- `blk->last_used`, `blk->desc_idx`

**Local order:** None. `inst->lock` is never nested under or over another kernel lock.

**Rules:**

- Submission, completion polling, and request allocation all run under `inst->lock`.
- The completion path calls `proc_make_ready(req->waiter)` while holding `inst->lock`. This is safe because the device lock is innermost and `proc_make_ready` only touches the runqueue side with its own locking.
- `virtio_blk_rw()` drops the lock before yielding the CPU when no slot is available.
- Do not call VFS, `kmalloc`, or blocking scheduler functions under `inst->lock`.

### virtio-net

**Lock:** `virtio_net_inst_t.lock` (`net->lock`), one per instance.

**What it protects:**

- TX/RX descriptor rings
- `tx_busy[]`, `rx_buf[]`, `tx_buf[]`
- Queue indices (`last_used`, `avail->idx`)
- Statistics (`rx_packets`, `tx_packets`, `rx_drops`, `tx_drops`)

**Local order:** `g_lwip_lock -> net->lock`.

- The lwIP raw-API send path is invoked while holding `g_lwip_lock`.
- `virtio_net_send()` with `nonblock == 1` is the only virtio-net entry point that may run under `g_lwip_lock`.
- The nonblock path never sleeps waiting for TX completion. It submits the descriptor and returns immediately.
- The blocking send/recv paths (`nonblock == 0`, or `virtio_net_recv()`) must NOT be called under `g_lwip_lock` because they can sleep.

**Rules:**

- Never hold `net->lock` across a scheduler wait.
- Never call lwIP functions while holding `net->lock` (the order is lwIP outer, net inner).
- `virtio_net_poll_all()` and `virtio_net_class_poll()` only acquire `net->lock`.

### UART

**Lock:** `rx_lock`, single global spinlock.

**What it protects:**

- Receive ring buffer (`rx_buffer`, `rx_head`, `rx_tail`)
- `rx_waiter`
- `tty_foreground_pgid`

**Local order:** `rx_lock -> proc_lock`.

- Normal RX handling only touches the ring buffer and `rx_waiter` under `rx_lock`.
- The Ctrl-C path (`uart_rx_push()` for `0x03`) holds `rx_lock` and then calls `proc_find()` / `proc_kill()` through `uart_signal_user_pgid()` / `uart_signal_all_user()`. These functions acquire `proc_lock`.
- `uart_dump_tasks()` also acquires `proc_lock` and is called under `rx_lock` in the Ctrl-C path.
- This is the only documented exception where a driver-private lock nests `proc_lock`.

**Rules:**

- All other UART paths must not acquire additional locks while holding `rx_lock`.
- `uart_getc()` drops `rx_lock` before blocking the current task.

### PTY

**Locks:**

- `g_pty_alloc_lock`: global spinlock for pair allocation and initial setup.
- `g_ptys[idx].lock`: per-pair spinlock for all runtime operations on a pair.

**What `g_pty_alloc_lock` protects:**

- `in_use` flag during allocation
- `m2s_buf` / `s2m_buf` allocation and initial field setup

**What per-pair `lock` protects:**

- Ring buffers (`m2s_*`, `s2m_*`)
- `master_refs`, `slave_refs`
- `locked`, `master_nonblock`, `slave_nonblock`
- Window size (`ws_row`, `ws_col`)

**Local order:** None. The two locks are never nested.

- `pty_alloc()` acquires `g_pty_alloc_lock`, allocates buffers with `kmalloc`, initializes the pair, and releases `g_pty_alloc_lock`. The per-pair lock is not held during allocation.
- All read/write/ioctl paths acquire only the per-pair lock.
- `pty_release()` acquires the per-pair lock to update reference counts and clear `in_use`, then releases the lock before dropping the backing `vfile_t` reference.

**Rules:**

- Do not hold the per-pair lock across VFS operations. Buffer I/O and reference dropping happen after the lock is released.
- `g_pty_alloc_lock` is held during `kmalloc`. This is a known exception; new allocations under this lock should be avoided if possible.

### loop

**Lock:** `g_loop[i].lock`, one per loop device.

**What it protects:**

- `in_use`
- `backing_vf`
- `backing_size`

**Local order:** None.

- `loop_set_fd()` obtains a `vfile_t` reference and size from VFS before taking the loop lock.
- `loop_clr_fd()` clears state under the loop lock, then releases the lock before dropping the `vfile_t` reference.
- `loop_dev_read()` and `loop_dev_write()` copy `backing_vf` and `backing_size` under the loop lock, release the lock, and then call the backing file's `lseek`/`read`/`write` operations locklessly.

**Rules:**

- The loop lock is never held across a backing-file VFS operation.
- No memory allocation happens under the loop lock.

### DW SDIO

**Lock:** None.

- The driver uses a single global `sdio_priv_t` instance (`g_sdio`).
- All command and data transfers are synchronous busy-polls.
- No private spinlock is required because only one request is in flight at a time and the current implementation does not support concurrent callers.

**Rules:**

- Future concurrent or IRQ-driven versions must add a private lock and document it here.
- Until then, callers serialize through the single instance.

### StarFive GMAC

**Lock:** None.

- The driver uses a single global `gmac_priv_t` instance (`g_gmac`).
- `starfive_gmac_send()` and `starfive_gmac_recv()` are register-polling paths that check descriptor ownership bits.
- No private spinlock protects descriptor rings in the current implementation.

**Rules:**

- Future IRQ-driven or SMP-safe versions must add a private lock and document it here.
- Concurrent send/recv calls race today; callers must serialize externally.

### Loongson-2K GMAC

**Lock:** None.

- The driver uses a single global `ls2k_gmac_priv_t` instance (`g_ls2k_gmac`).
- `ls2k_gmac_send()` and `ls2k_gmac_recv()` are register-polling paths that check descriptor ownership bits.
- No private spinlock protects descriptor rings in the current implementation.

**Rules:**

- Future IRQ-driven or SMP-safe versions must add a private lock and document it here.
- Concurrent send/recv calls race today; callers must serialize externally.

## Cross-driver rules

1. **No reverse ordering.** If a global lock must be acquired while a driver lock is held, document it as a local order in this file. Undocumented nesting is a bug.
2. **Blocking under spinlocks is forbidden.** Any path that may block must drop all spinlocks first.
3. **VFS and allocation under driver locks are forbidden unless documented.** The only documented exceptions are:
   - `virtio-blk` completion calling `proc_make_ready` (scheduler notification, not VFS).
   - `UART` Ctrl-C path nesting `proc_lock` under `rx_lock`.
   - `PTY` allocation performing `kmalloc` under `g_pty_alloc_lock`.
4. **New device locks** must either fit the global order (`driver registry/IRQ locks -> device-private locks`) or add a local order entry to this document before use.

## References

- `kernel/include/core/lock.h` — global lock order and spinlock primitives.
- `kernel/drivers/block/virtio_blk.c` — `inst->lock` implementation.
- `kernel/drivers/net/virtio_net.c` — `net->lock` and `g_lwip_lock` interaction.
- `kernel/drivers/char/uart.c` — `rx_lock` and Ctrl-C signal path.
- `kernel/drivers/char/pty.c` — `g_pty_alloc_lock` and per-pair `lock`.
- `kernel/drivers/block/loop.c` — per-device loop lock.
- `kernel/drivers/block/dw_sdio.c` — lockless SDIO implementation.
- `kernel/drivers/net/starfive_gmac.c` — lockless StarFive GMAC implementation.
- `kernel/drivers/net/ls2k_gmac.c` — lockless Loongson-2K GMAC implementation.
