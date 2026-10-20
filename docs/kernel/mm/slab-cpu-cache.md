# Slab CPU 数组在 refill/drain 期间的归属

小对象分配器的快路径在关中断、且不持 slab cache 锁的情况下访问自己的 CPU 局部数组；慢路径在整段 refill 或 drain 期间也必须保持同样的 CPU 归属。

此前 `kmalloc_flags` 选中一个空的局部数组后重新开中断，再取 slab cache 锁去 refill 先前选中的那个数组。这个窗口里若发生迁移，任务就会去访问另一个 CPU 的数组，而那个 CPU 的快路径可能正在改同一个数组。`kfree` 在「观察到局部数组已满」与「排空 8 项」之间也有同样的窗口。cache 锁挡不住一个刻意绕开该锁的快路径。

两条路径现在都把最初的关中断保持到这段有界的 cache 事务结束，解锁后再恢复调用者原来的中断状态。不需要任何架构专属策略。

```sh
python3 tools/tests/test_slab_cpu_migration.py
```

该 host 探针抽取真实的 refill/drain 代码，在中断已开启的时刻注入迁移，断言两条路径都保持 CPU 归属，并覆盖本来就关中断进入的调用者。x86_64 全 VM 冒烟与 RISC-V 四 CPU 抢占式下的 `mtcorrupt_test`、`vm_fault_test`、描述符传递与网络选项探针同样覆盖这些路径。

在本修复之前的一次浏览器运行中抓到过一个跨不同 vnode 的脏页链环：一个 CPU 持页缓存锁遍历该环，其他 CPU 在关中断下自旋，随后 COW shootdown 超时。slab 迁移竞态是一个已被证明会破坏无关对象的分配器缺陷；那个环的首次写入没有被抓到，因此浏览器冷启动验证必须单独记录，不能拿这个 host 探针当作该特定故障成因的证明。
