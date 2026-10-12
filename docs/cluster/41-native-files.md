# Native 程序文件读取与多处理器共同执行

## 已实现的行为

同一个 Native 程序现在可以在共享 MCU 内核和 RV64、AArch64、x86 服务器中，
通过既有 `path_open`、`handle_read`、`handle_seek`、`handle_stat`、`handle_close`
接口读取文件。用户程序不包含 STM32、块设备或文件系统相关分支。系统调用参数
验证、对象类型、权限、用户缓冲区复制和句柄生命周期共同位于
`kernel/abi/native/sys_file.c`。普通服务器继续使用原 full VFS。

精简 flat 内核的文件后端是可选的 `NATIVE_FLAT_FILES=1`，当前构建要求同时
启用 `MCU_NATIVE_PROGRAM=1`。它复用生产 `vfile` 分配、引用、身份和 Native
句柄表，后端使用公共 `fat32lite`。板级代码提供 `fat32lite_io_t.read` 扇区
读取回调，再调用 `a20_flat_files_bind(io, partition_lba)`；接口没有板卡或
ISA 编号。回调上下文及只读介质必须存活到内核结束，绑定后不能卸载或重新绑定。
允许读取回调阻塞，共享 sleepable mutex 串行化文件系统 scratch sector 和
文件偏移操作。未启用此配置时没有文件后端、锁或周期服务。

flat 配置目前支持只读 FAT32 和 fat32lite 的短文件名路径，文件大小使用 32 位值，seek
偏移上限为 2 GiB−1。打开要求 `dir=A20_HANDLE_NULL`、flags=0、mode=0；0 是
有效 Native 句柄，不能当作 NULL。提供 READ/STAT/SEEK 权限并允许调用者缩减
权限。写入、创建、目录句柄、mmap、动态库和路径可执行文件装载尚未提供。
没有因此宣告完整 HANDLES 能力，执行能力发现尚未增加单独的文件支持位。
普通 full VFS 的目录、写入及其他功能不受这个配置限制。

## 实际用户程序验收

`user/tests/native-file-contract.h` 是两类内核共用的用户态代码。它实际打开
`/bin/INPUT.DAT`，查询 2048 字节大小，以两个 iovec 读取，核对全部字节及
FNV32=60f4edc5，再检查 EOF、跨 512 字节扇区边界的 seek/read、关闭后拒绝
使用旧句柄、限制权限以及缺失文件。另一个公共线程用例检查对 THREAD 句柄
读写返回 INVALID_ARGUMENT，随后仍能等待线程正常退出。

同一程序中的两个工作线程分别打开和读取文件，完成同步后继续竞争领取
[40](40-native-application-balance.md) 的 32 个计算任务。核验没有重复和遗漏、
总和 4359168，并在双处理器运行时要求实际 CPU 掩码为 3。文件读写没有替代
实际计算和调度验收；现有线程迁移、共享堆、futex、应用 owner 与退出资源
回收检查仍保留。

服务器额外连续执行 4100 次真实打开/关闭，超过每进程 4096 个 fd 的上限。
本阶段发现旧 Native open 为句柄取得引用后没有释放其临时 fd，关闭句柄仍会
泄漏 fd。修复在成功和失败时都关闭临时 fd，与现有 path_create 的所有权
规则一致。父提交 f6b94b9d 的真实 RV64 内核运行新增用户程序，首次文件读取
通过，反复打开关闭失败；当前内核通过全部 4100 次。此负向运行保留日志和
旧内核哈希，用户 FAT 未在覆盖前单独冻结，不能声称其完整磁盘二进制已归档。

## 构建和复现

```sh
python3 tools/check-native-sync.py --cpus 2 --timeout 180 \
  --output .kernel-build/cluster/native-files-server-complete
python3 tools/check-cluster-apps.py --timeout 600 \
  --log-dir .kernel-build/cluster/apps-native-files-complete

make CCACHE= ARCH=armv7m BOARD=stm32f103 ABI=native \
  STM32_FLASH_KB=512 STM32_RAM_KB=64 STM32_QEMU=0 STM32_XUANWU=0 \
  STM32_CLUSTER_PORT=0 NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 \
  CONFIG_KERNEL_PREEMPT=1 MCU_SHARED_KERNEL=1 MCU_SHARED_RAM_KB=1024 \
  MCU_SHARED_TEST=1 MCU_NATIVE_PROGRAM=1 NATIVE_FLAT_FILES=1 kernel-only
# 使用已安装 Unicorn/pyelftools 的 Python；RTL backend 路径见 25/26。
python3 tools/check-mcu-program.py <上一步 kernel.elf> \
  --backend <Va20_shared_top> --seed 22 --rounds 12000000 \
  --report .kernel-build/cluster/native-files-mcu-complete.json
```

MCU selftest 通过 `tools/make-native-file-fixture.py` 制作真实 64 MiB FAT32
逻辑磁盘，保存完整磁盘哈希，再把全部非零扇区作为只读 Flash fixture 链入
固件；零扇区由回调返回零。该载荷约 12 个非零扇区，不能把逻辑磁盘大小算作
MCU RAM/Flash 实际容量，也不是物理 SD 控制器测试。实际文件解析、用户读取、
异常和系统调用执行都在 MCU 固件中，宿主不代替用户程序计算或写入结果。
自测以外的硬件需提供真正的扇区驱动和足够的存储，当前没有物理多板证据。

## 普通运行与验收边界

默认精简 MCU 内核 SHA256 仍为
`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。
本阶段改变普通服务器的代码布局并修复 fd 泄漏，服务器二进制会变化；没有
新增普通程序的后台文件服务，也没有完成全量性能测量。文件代码共用不意味
多个网络域自动共享本地磁盘，跨域程序启动继续使用 [37](37-application-execution.md)
的 ISA 变体和应用 owner。

[39](39-application-recycling.md) 记录的 AArch64 偶发 init 引用下溢仍未定位，
本轮通过不能作为其修复。当前文件支持是程序运行时的一部分；Java 解释器、
类库、本地显示/输入以及 [24](24-single-system-minecraft.md) 要求的物理多 MCU
真实 Minecraft 客户端仍未完成。

本阶段最终报告、实际内核/用户 ELF、MCU 文件 fixture、源文件哈希和失败尝试
归档到 `.kernel-build/cluster/evidence/native-files-<commit>/manifest.json`。
重叠运行、被后续构建覆盖的中间报告作为过程记录，不代替最终冻结二进制验收。

## 2026-10-13 最终验收

| 执行环境 | 实际结果 |
|---|---|
| RV64 / AArch64 / x86 双核真实内核 | 文件读取、4100 次打开关闭、两工作线程读文件、32 项计算；每种 CPU 掩码均为 3，通过 |
| 双 ARMv7-M 固件指令 + 共享 RAM RTL | 同一文件/线程接口、17 次打开关闭、32 项计算掩码 3、五次任务退出、最终堆恢复，通过 |
| 三 ISA 网络集群 | 六个真实程序分配为 2/2/2、48 次退出/回收、12 次 CLI 退出 37、一次远程停止退出 137，通过 |
| 原无文件 MCU 应用配置 | 原 Native 程序完整流程通过；该项运行未连接 RTL |
| 默认精简 MCU | 实际构建 kernel.bin 的 SHA256 与父提交基线完全一致 |

ABI、IDL、裁剪注册和文档门禁通过。格式门禁仍为父提交同一组 114 个既有
失败路径，没有新增或扩大豁免。最终 MCU 内核 ELF SHA256 为
`c173c2937e63361f950f6b6bacd315126c1ca999ef376deb1399d54498fa3e82`。
服务器报告逐项记录内核及磁盘哈希，最终冻结镜像与报告匹配。所有上述
MCU/服务器执行均为模型或虚拟机证据，不替代物理 MCU 或真实 Minecraft 客户端验收。
