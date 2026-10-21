# arm32 的 FP 上下文

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[CORE-07](../core.md)。

判定标准是「用户态是否启用 FP/SIMD」：arm32（`-mfpu=vfpv3-d16 -mfloat-abi=hard`）、riscv32（`ilp32d`）、
ppc64le（`MSR_FP`）都启用；loongarch32/armv7m 无 FPU，不受影响。

## 现在的状态（2026-10，本轮实测）

- arm32 现在能编过也能启动：`a20os-buildenv` 容器里 `make ARCH=arm32 ABI=linux BRINGUP=0 dev-build`，
  QEMU `-machine virt -cpu cortex-a15 -m 1G` 启动到 shell、执行命令、`poweroff` 后 `audit errors=0`。
  启动路上修掉的两项（`PHYS_MEMORY_END` 与 1 GiB 启动参数、`a20_elf_load_transaction_ops_t` 的
  ILP32 padding）见 [mm.md](../mm.md) MM-05 与 CORE-07 的提交说明。
- EL0 的 VFP 本身可用：`user/cmds/core/fp_touch.c` 在 arm32 上 `FP_TOUCH: PASS`（CPACR cp10/cp11 与
  FPEXC.EN 在 `kernel/arch/arm32/boot/entry.S` 已开）。kernel 与 userland 都是 hard-float。
- **trap 帧没有 FP 状态**：`trap_context_t` 目前 84 字节（GPR 部分），没有 `d0-d15`/FPSCR，所以两个任务
  共享同一份物理 FP 寄存器文件。

## 试过什么、为什么撤回

在 trap 帧尾部加 VFP 块（`TF_SIZE` 84 → 224、`TF_VFP_D` 88、`TF_FPSCR` 216，帧拷贝循环从 3 对
`ldmia/stmia` 改成 7 轮 8 字，两侧返回路径各加 `vldmia`/`vmsr`），内核能编过，但**第一次用户态 syscall
之后整机挂死**（`undef_vector: b .` 的形态：无输出、无 panic）。已排除的：IRQ 上下文里的 `vstmia` 正常
（每次 tick 都走，启动能过）；单独禁用用户返回路径的恢复仍挂；帧尺寸/银行栈保留（240 < 4096）与
`task_context_t` 偏移都没变。未定位，改动已撤回，没有留在树里。用 `gdb-multiarch` 连 QEMU 桩看过挂死现场：CPU 停在
`arm32_vector_table+12`（prefetch abort 槽）且 `CPSR` 模式位是 ABT(0x17)、I/F 都屏蔽，即向量表本身
在当时的翻译机制下取指失败，向量页不可达 → 反复 abort 自旋；既没有 panic 也没有输出，与观察一致。
下一步应从"陷阱入口/返回在 ABT 模式下把 TTBR0 留在了哪张页表"入手，而不是继续猜 FP 指令。

## 用例

`user/cmds/core/fork_fpu_probe.c` 的 arm32 分支：父进程装 sentinel → 子进程装另一组 → 父进程读回自己的
`d0/d15`。当前内核上的实测结果就是缺陷本身：

```
FORK_FPU: arm32-child-registers PASS
FORK_FPU: FAIL parent d0=0x2222222222222222 want 0x1111111111111111
```

父进程读到的是子进程的 sentinel——两个任务共用一份物理 FP 寄存器文件。子进程用的是自旋而不是 `usleep`，
因为 **arm32 的 `sleep`/`nanosleep` 不唤醒**（`sleep 1` 永不返回，本机实测）——那是同一个移植面上的
独立缺陷，尚未记账。
