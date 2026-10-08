# lwIP 共享分配器的 SMP 保护

多 lane 核心处理可以同时调用 `mem_malloc`、`mem_trim`、`mem_free` 及 memp/pbuf
接口。`MEMP_MEM_MALLOC=1` 时，lane descriptor 与计数上下文仍共用一个 lwIP heap，
因此仅有 PCB 分桶不能保证分配器安全，也不能把该配置描述为物理内存池分片。

`CONFIG_NET_LANES>1` 下，heap free-list 使用独立的 IRQ-save 自旋锁；trim 必须取得
锁后才读块链接和大小。`SYS_ARCH_PROTECT` 使用另一把短 SMP 锁，按 CPU 保存嵌套深度，
用于引用计数、memp 计数、统计更新及 loopback 队列发布。锁序为 heap → protect；
反向进入 heap 会直接触发断言。获取 CPU 身份前先关闭本地中断，避免迁移使嵌套状态错位。
单 lane 下保留原有 IRQ-save 保护语义。

`make host-tests` 包含 `test_lwip_allocator_concurrency`：实际编译 vendored
`mem.c`、`memp.c` 和 `pbuf.c`，8 个宿主线程各执行 20,000 轮分配、填充、trim、校验、
释放与共享 pbuf 引用操作；结束时检查 heap/pool 使用量及错误计数。宿主端仅替换类型
与锁原语为 pthread 实现，所以它验证分配器算法并发行为，内核 IRQ/锁实现另由 QEMU
网络门禁验证。本轮宿主测试通过，日志为本机 `/tmp/a20-recovery-logs/lwip-host-tests.log`。
