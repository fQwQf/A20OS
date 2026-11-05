# 同一应用的动态工作领取与实际 CPU 分摊

## 直接使用

`user/liba20rt/a20_work.h` 提供共享地址空间内的工作范围。MCU 与服务器使用同一接口，已包含在 `a20_sdk.h` 中，不新增 syscall，也不要求用户指令集支持跨芯片原子指令。底层复用公共 `atomic32` syscall；共享 MCU 内核由现有 fabric 仲裁保证原子操作，服务器由现有内核同步路径保证。

```c
a20_work_range_t work;
/* 在创建/发布工作线程前初始化；每个索引表示应用自己的一个待办任务。 */
a20_status_t status = a20_work_range_init(&work, 0, item_count);
if (status != A20_OK)
    return status;

/* 每个 Native 工作线程执行相同循环，无需指定目的 CPU。 */
for (;;) {
    uint32_t index;
    status = a20_work_range_take(&work, &index);
    if (status == -A20_ERR_WOULD_BLOCK)
        break;
    if (status != A20_OK)
        return status;
    compute_item(index);
    /* 示例采用协作让出；应用也可沿用正常抢占调度。 */
    a20_thread_yield();
}
```

线程创建、栈、等待和句柄关闭沿用 [32](32-native-threads.md)。工作线程的允许 CPU 集合应包含需要参与的在线 CPU；不需要逐线程绑定 CPU。领取成功只分配一个索引，多个线程不会领取同一个索引。处理较快的线程可以继续领取下一项；不预先固定每个线程的任务份额，也不承诺相等的任务数或执行时长。

范围为半开区间 `[begin, end)`，`begin == end` 立即耗尽，`begin > end`返回 INVALID_ARGUMENT。即使 end 为 UINT32_MAX，游标也不会回绕。耗尽或 syscall 错误不修改输出索引。初始化和 end 字段修改必须发生在没有工作线程使用范围时，随后通过线程创建或公共同步操作发布。成功领取后的任务由应用负责完成和记录；工作线程故障不会自动重新提交，耗尽不等于全部计算完成，调用者仍应 join 并核验结果。

该接口操作一个共享地址空间中的工作范围。跨网络执行域继续使用实际应用启动接口 [37](37-application-execution.md)，按兼容 ISA 和活动程序数选择执行域；不是让不同 ISA 直接执行彼此的原生线程栈。网络域中的程序与共享内核中的线程分摊是两个已经可执行的层次，均沿用公共 Native 接口。

## 实际计算验收

`user/tests/native-balance-contract.h` 是 MCU、RV64、AArch64、x86 共用的用户态程序代码。两个真实用户线程都允许使用完整在线 CPU 集合，先同步开始，再竞争领取 32 项任务。每项从共享输入数组读取 1…128，按任务编号计算。程序核验两线程的任务位图无交集且覆盖全部 32 项、总数 32、结果和 4359168，并通过公共 `thread_get_cpu` 记录实际执行所在 CPU。双核验收必须观察到两个 CPU；单核验收必须仍能完成相同程序。

旧有的显式亲和性迁移检查保留，发生在工作领取之前，不充当本项分摊证据。测试另外检查空范围、无效范围、UINT32_MAX 边界和耗尽后输出保持。完整流程继续覆盖嵌套线程、join、句柄身份、共享堆、futex 和退出后资源回收。

```sh
python3 tools/native/check-native-sync.py --cpus 2 --timeout 120 \
  --output .kernel-build/cluster/native-work-range-dual
python3 tools/native/check-native-sync.py --cpus 1 --timeout 120 \
  --output .kernel-build/cluster/native-work-range-single
make CCACHE= PYTHON=<Unicorn/pyelftools Python> check-mcu-program-rtl
python3 tools/check-cluster-apps.py --timeout 600 \
  --log-dir .kernel-build/cluster/apps-native-balance
```

服务器检查器要求 `NATIVE_BALANCE: PASS jobs=32 sum=4359168 mask=… range=dynamic`； MCU 检查器要求对应的 PROGRAM-BALANCE 标记，并继续核验真实 SVC、两处理器用户指令、共享 RAM RTL 访问和最后资源回收。报告明确标记模拟运行，物理板不能用这些报告替代。

## 普通运行与尚未完成项

工作范围是显式使用的头文件接口，不自动创建线程、服务或周期扫描。原来不使用它的程序不增加工作领取调用。默认精简 MCU 构建 SHA256 仍为`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。

本阶段也补充任务引用下溢的失败现场输出：任务地址、状态、队列和 CPU 所有权等格式化位于 cold/noinline helper，只在已判定引用错误时调用，没有新增正常路径锁、扫描或校验。服务器内核因此重新链接，不能声称二进制不变；没有全量性能测量。这项诊断不是 [39](39-application-recycling.md) 中 AArch64 偶发 init 引用下溢的根因修复，后续通过不能关闭该问题。

本项实现可运行程序中的共享工作分配，不等于 JVM 或 Minecraft 已运行。物理多 MCU、其他 MCU 的原生用户上下文适配、类库、文件/显示/输入运行时和真实 Java 客户端验收仍按 [24](24-single-system-minecraft.md) 继续推进。

## 本阶段故障复现

新增计算后的最终 MCU 流程曾在应用终态检查失败。冻结的原失败 ELF 以相同交错重新运行，指令观察点只读取得：根任务 state=UNUSED、destroy_started=0、 exit_code=0、on_cpu=0、refs=5；适配器却返回 RUNNING、未释放 owner 引用。公共 reaper 先从任务表摘除并设 UNUSED，再调用 destroy，两个动作间允许并发观察。原测试适配器把 destroy_started 误当作退出条件。

修正仅作用于显式 MCU selftest 适配器：对已发布且持有引用的根任务，离开 CPU 后的 ZOMBIE 或 UNUSED 都表示已退出，按原规则保留退出码并释放一次引用。没有更改公共 reaper 或调度器。检查器现在将运行失败写为 FAIL 和实际 ELF 哈希；冻结失败 ELF、复现脚本、字段偏移及观察结果一并归档。此问题与尚未定位的 AArch64 init 引用下溢分别记录，不把其中一个修复当作另一个的修复。
