# RISC-V 虚拟 SBI：接口与验证

RV32/RV64 的 VS supervisor `ecall` 由`kernel/arch/common/riscv/hyp_sbi.c` 统一处理。Guest 的 VU `ecall` 委托给自己的 VS 内核，不作为 SBI。两种后端继续要求 H、Sstc 和 firmware STCE 权限。

## 身份与兼容范围

BASE 报告 SBI 0.2（编码 2）、implementation version 2；implementation ID `0xa20` 仅为本地选择，**尚未获上游分配**。SRST、DBCN 作为可探测的较新扩展提供；这不是完整 SBI 2.0 合规声明。只支持按版本门禁发现 DBCN 的 guest （例如要求 specification >= 2.0 的客户端）不会自动启用它；应显式 probe，或使用已支持的 legacy console / UART。没有为了启用客户端而虚报规范版本。

| 扩展 | 当前契约 |
|---|---|
| BASE `0x10` | FID 0–6，版本、实现信息、扩展探测；三个 machine ID 为合法的虚拟值 0 |
| TIME `0x54494d45` | FID 0，64 位绝对时间；RV32 从 a0/a1 拼接。设置虚拟比较器并清软件注入的 timer pending |
| HSM `0x48534d` | FID 0–2，启动、停止、状态。无效 hart 为 -3，无效或 NX 启动地址为 -5，已启动为 -6 |
| IPI `0x735049` | FID 0，VM 内 hart mask/base；RV32 的 base=-1 正确扩展为广播语义 |
| RFENCE `0x52464e43` | FID 0–2，指令及 VS 翻译刷新；采用全范围、全 ASID 刷新覆盖请求。活动远端通过 firmware 同步刷新，非活动 vCPU 在重新 load 时刷新 |
| SRST `0x53525354` | FID 0，shutdown 类型 0；reason 支持 0/1。保留或未实现参数返回 -3，cold/warm reboot 返回 -2；成功关闭整个 VM |
| DBCN `0x4442434e` | FID 0/1/2，批量输出、批量输入、阻塞输出单字节 |
| Legacy 0/1/2/8 | timer、putchar、getchar、shutdown。getchar 无输入为 -1；a1 保持不变，a6 被忽略 |

BASE 的 extension probe 同时报告以上四个 legacy EID。未知 EID/FID 返回`SBI_ERR_NOT_SUPPORTED`，不终止 VM。现代调用只改变 a0/a1，legacy 只改变 a0；PC 前进按 guest XLEN 回绕。阻塞输出遇到 managed console 满队列时返回 host YIELD，保留原 PC 和参数，待 VMM 排空后重试，不重复已经完成的输出。

## DBCN 内存与调度边界

缓冲区是 guest **物理地址**，不能作为宿主指针使用。RV32 使用 low/high 两个 XLEN 参数表示物理地址，RV64 的 high 必须为 0。只接受配置 RAM 窗口内的缓冲区；超范围、地址溢出、MMIO 和权限不足均拒绝。

单次缓冲区最大 64 KiB；更大的请求返回 INVALID_PARAM。此实现限制使关闭中断的 trap 上下文中的整段权限检查有界。每次调用实际最多处理 256 字节，调用方按 a1 返回的已处理长度推进地址并继续调用；输出满或输入空可成功返回 0。使用整个请求的权限预检查，并在实际访问时，在页表锁下同时校验权限和 pin backing，拒绝通过输入覆盖只读代码页。输出需要 R，输入需要 W。未映射 RAM 按既有 demand-fill 策略分配并记账，不访问 guest 未拥有的宿主内存。

控制台使用 VM 私有的输入输出队列和既有 console_lock；DBCN 与 legacy getchar 共用 UART 的 guest 输入队列。该路径未提供虚拟外部中断控制器。

## 实际验证

QEMU TCG 10.0.13，GCC 14.2.0，ABI=both，NR_CPUS=2，当前源码构建：

- RV64/RV32 各通过 40 项真实 VS/VU 调用检查：BASE、错误返回、寄存器哨兵、 TIME 清 pending、RV32 时间高字、HSM 拒绝 NX 入口、IPI 投递、RFENCE、SRST 参数、DBCN 跨页收发和 VU 系统调用委托。
- 双宿主 CPU / 双 vCPU 同时 RUN：guest 发起远程 IPI，目标 guest 检查 sip； guest 发起远程 FENCE.I / SFENCE.VMA / ASID fence，检查 HSM 状态和重复 start 错误，两个 hart 最后执行 SBI hart_stop。此测试的 secondary 仍由宿主预启动，不宣称已覆盖 guest 从 STOPPED 状态首次启动 secondary 的全部竞态。
- VM/vCPU/映射页/页表页计数回到基线；既有 NX/RO、stage-2、并行回收测试通过。
- RV64/RV32 关闭 H 后明确 SKIP，宿主正常关机。
- 六项 Python 测试方法通过（含 RV32/RV64 子场景）：生产分发器错误路径、背压重试、权限拒绝、RV32 高 GPA 参数、后端可用性和 NoMMU 拒绝访问。

附加跨架构检查未全部通过：ARM32 因缺少 `arm-linux-gnueabihf-gcc` 未构建； x86 的 stage-2、NX/RO、抢占和三轮单 vCPU 通过，但双 vCPU 会合出现`ready=0,1 peak=1` 超时。将公共 `hyp_test.c` / `hyp_dev.c` 恢复为 HEAD 版本、保留其它对象重新链接的局部对照也出现超时（较早的一次同类对照通过）。该对照不是干净 HEAD 的全树构建，工作区同时存在其它 MM/COW 修改，故尚不能归因；当前不宣称 x86 SMP 回归通过。新增 SBI 专项由 `ARCH_HAS_HYP_SBI` 能力开关仅在 RISC-V 编译，非 SBI 后端保持原测试结构与 UART 轮询路径。

复现：

```sh
python3 -m unittest tools.tests.test_hyp_sbi tools.tests.test_hyp_s2 \
  tools.tests.test_hyp_backend_contract tools.tests.test_riscv_hyp_nommu -v
python3 tools/qemu/check_hyp_multiarch.py --arch riscv64 --cpus 2 --build
python3 tools/qemu/check_hyp_multiarch.py --arch riscv32 --cpus 2 --build
# 对刚构建的对应内核，可追加 --without-virtualization 验证无 H 扩展时的安全回退行为。
```

通过标记：`HYP_SBI_TEST: PASS xlen=32/64 checks=40`、`HYP_SBI_SMP_TEST: PASS remote-ipi+rfence+hsm` 和原有 `HYP_SMP_TEST: PASS`。验证测试清单 记录镜像、源码与归档日志哈希。

## 剩余缺口与边界

HSM suspend/resume、guest cold/warm reset、PMU、system suspend、nested H 扩展及其 RFENCE FID 3–6 未实现，当前测试不代表完整 Linux guest 或完整 OS SMP 验收。进一步申请 implementation ID 前，应继续做所选规范基线的逐条审计，尤其是 HSM start/stop 并发、跨核地址翻译缓存可见性和长期资源回收。

构建修正：RV64 SMP 构建的 C906 cache probe fixup 在 SMP 禁用 probe 时不再引用被优化删除的局部汇编标签；保留单核 C906 的缓存维护行为。

规范参考：[调用约定](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/binary-encoding.adoc)、[HSM](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-hsm.adoc)、[SRST](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-sys-reset.adoc)、[DBCN](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-debug-console.adoc)。
