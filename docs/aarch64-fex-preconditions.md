# AArch64 FEX 相关内核接口审计

`tools/a20 test check-fex-precond-aarch64` 运行一条 AArch64 Linux ABI 实例，检查两组 FEX 相关接口：exec 初始栈上的 auxv、`/proc/self/auxv` 的格式，以及 `mmap`/`mprotect` 对 `PROT_BTI`、`PROT_MTE` 和未知保护位的处理。

**这条门禁是接口审计，不代表 FEX 已能在 A20OS 上运行。** 它没有构建或启动 FEX，没有验证 x86-64 guest、FEX RootFS、JIT、动态链接器、FEX 所需系统调用，也没有证明宿主 CPU 特性探测与 Linux 行为完整兼容。上游 FEX 项目将 AArch64 Linux 作为宿主，并列出 ARMv8.0-A、FP、CRC32 等要求；满足少数内核接口只是其中一部分。[FEX 官方项目说明](https://github.com/FEX-Emu/FEX)

## Auxv：两种接口分开判断

内核在新进程初始栈上提供原生字长的 `{type, value}` 二元组，末尾为 `{AT_NULL, 0}`。当前 ELF 初始化路径共放入 19 组，包含一个 `AT_NULL`；探针在初始栈上确认 `AT_PHDR`、`AT_PHENT`、`AT_PHNUM`、`AT_ENTRY`、`AT_PAGESZ` 各出现一次，并检查页大小为 4096。当前初始栈**没有 `AT_EXECFN`**，探针会把它明确打印为 absent。

`/proc/<pid>/auxv` 是另一项 Linux 接口。Linux 文档规定它返回原生 `unsigned long` 的 ID/value 序列，结尾为两个零。[Linux `proc_pid_auxv(5)`](https://man7.org/linux/man-pages/man5/proc_pid_auxv.5.html) A20OS 现在也以当前 ABI 的原生字长输出二进制 pair；探针以 7 字节短读跨越 pair 边界，检查文件偏移、seek 到 EOF 后的 EOF 读取、rewind、总长度、每个 pair 与 exec 初始栈完全一致，并单独验证 `AT_PHDR`、`AT_PAGESZ` 和唯一的 `{AT_NULL, 0}` 终止项。内核读取时在 task 的 `park_lock` 下 pin 住 `mm`，随后读取 `mm` 锁保护的 auxv 快照，避免与并发 exec/exit 竞争。

因此本门禁的 auxv 结论是 `PROC_LINUX_COMPAT=YES; FEX_SUPPORT=NOT_ESTABLISHED`。这只表示当前覆盖到的初始栈和 `/proc` auxv 格式及字段符合探针断言；`AT_EXECFN` 仍未提供，FEX 的其他系统调用、RootFS、JIT、动态链接器和 CPU 要求也未验证，所以不能推导 FEX 整体兼容。

## `mmap` 与 `mprotect` 的保护提示

`PROT_BTI`（`0x10`）和 `PROT_MTE`（`0x20`）是提示位，不是读写执行权限。当前内核接受它们并在权限策略及页表处理前去掉；这只证明调用不会因提示位得到 `EINVAL`，**不表示 A20OS 实现了 BTI landing-pad enforcement 或 MTE allocation tags**。`mmap` 当前没有完整校验保护位，探针只比较带/不带提示时映射是否同样成功，不把它描述成提示已被实现。

对 `mprotect`，探针分别要求无提示、BTI、MTE 及组合提示成功。未知位 `0x40000000` 则必须以 `-1` 且 `errno == EINVAL` 失败；任意其他失败都算探针失败。成功映射还要能实际写入，避免只检查 syscall 返回值。

实例定义在 `instances/check-fex-precond-aarch64.toml`，探针是 `user/cmds/core/auxvprobe.c` 与 `user/cmds/core/btiprobe.c`。门禁日志保留具体字段和 errno，便于继续补足 Linux auxv 标签或扩展 FEX 端到端测试，而不会把“已忽略提示”记作完整兼容。
