# Fork 与 mprotect 的 COW 并发约束

fork 复制私有映射时，父页表的叶节点锁保护源 PTE 快照、子映射所需的帧引用和父 PTE 的 COW 降权。子页表映射在释放父叶节点锁后完成，避免在节点锁内分配页表；若子映射失败，父映射保留 COW 仍可安全写入。

`mprotect` 在写入权限前重新核对它先前观察到的 PTE。若并发 COW 已替换该项，它会重试同一虚拟地址。对 fork 后仍有多个引用的私有匿名页恢复写权限时，PTE 保留 COW，第一次写入仍会复制页面。

状态驱动的匿名 COW 路径在叶节点锁内核对 PTE 与状态字节，并在释放叶锁前持有旧帧引用；复制结束后通过比较并替换安装新帧。该路径与处理文件页及独占帧的回退路径共用地址空间锁，保证两个实现不会同时改写同一映射。

`smoke-mm-fork-exec-race` 除并发 fork 与 VMA 操作外，还运行确定性隔离检查：子进程对 fork 共享页依次执行只读和读写 `mprotect`，再写入该页；父进程在每轮后确认原值未改变。该检查覆盖恢复写权限不能清除仍有效的 COW 义务。

`pfn_valid()` 只表示 PFN 落在已登记的物理内存范围内，不检查帧当前是否分配或其引用数。帧的存活由 `pfa.meta[].refcount` 及持有的映射/临时引用保证；因此，PFN 范围检查本身不构成并发帧回收保护。

## RV64 内核 trap 的寄存器保存

RV64 内核态 trap 入口在分配 trap frame 后，需要先保存原始 `t0`，再借用它重建陷入前的 `sp`。通用寄存器保存循环跳过已单独保存的 `x5/t0`。此前循环把临时计算出的旧 `sp` 写进了 `t0` 槽，导致 timer IRQ 返回后被打断的内核代码拿到错误的 `t0`；context switch 路径中 `t0` 可暂存地址空间状态，错误恢复会扰乱后续控制流或页表切换。

`make smoke-rv64-trap-t0` 通过 `a20.trap_t0_selftest=1` 在 RV64 单核 guest 中运行哨兵测试。测试在 `t0` 持有固定值时等待本 CPU 的 supervisor timer IRQ，并要求中断处理程序确认 IRQ 确已发生且汇编探针返回时哨兵未变；超时、未收到本 CPU 的 IRQ 或寄存器值变化都会失败。MM 修复后的回归还应运行 `make -j8 smoke-mm-pt-race`、`make -j8 smoke-mm-fork-exec-race` 和 `make -j8 smoke-mm-stress`。
