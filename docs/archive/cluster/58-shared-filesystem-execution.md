# 跨实例文件系统与磁盘程序执行

在 [57](57-shared-disk-execution.md) 的真实块设备路径上，现已验证普通 Linux 用户程序在 CPU 1 挂载主 QEMU 独有磁盘上的 ext4，从该磁盘 exec 子程序，写入并同步文件、重命名、同步目录、回读和卸载。两实例仍运行同一个原生 A20OS 内核，次实例没有磁盘或 UART，使用系统已安装的 QEMU/KVM。

## 实现

`mount_setup_block_device()` 原来在普通 BRINGUP 配置中恒返回 NULL，即使可加载驱动已经探测出设备，显式 mount 也无法取得它。现将既有 class-device 适配层保留在所有配置中，自动根挂载仍由原配置控制。没有新增集群 mount、 exec 或文件调用；普通模式使用相同适配层与已有直接硬件访问。

`cluster_threads --console --filesystem` 完成原有双线程迁移/计算/内存权限测试后，将主线程绑定 CPU 1：

1. 显式挂载 `/dev/vda` 到 `/mnt`；磁盘为检查器新建的 32 MiB ext4。
2. fork 后 exec `/mnt/program --filesystem-child`。该程序由宿主在 mkfs 时放入磁盘，子进程直接确认亲和性继承后仍在 CPU 1。
3. 子程序创建文件并写入 65536 字节，调用文件 fsync，关闭后重命名，再打开目录并调用目录 fsync，以退出码 37 结束。
4. 父程序 waitpid 验证退出码，重新打开文件逐字节回读、确认 EOF、卸载。
5. 检查器等待本次所有 QEMU 停止，用宿主 debugfs 独立提取结果，并检查执行文件的 SHA256 未变，再用 e2fsck 检查磁盘元数据。

这里使用真实文件系统的 fsync 路径，区别于上一阶段原始块节点的 flush ioctl。该验收没有做断电、设备丢失或 owner 故障恢复；不可由正常同步和卸载推出这些能力已完成。

## 验收发现的 ext4 问题

首次运行中的文件与程序行为正确，但宿主发现块组校验和、总空闲块/节点计数不一致。普通 SMP 对照也复现了相同问题，说明是既有 ext4 写回问题，不能将它隐藏在共享设备后端内。

修复包括：

- 非 metadata_csum 路径也更新总空闲计数；启用 GDT_CSUM 时更新 CRC16。
- 32/64 字节块组描述符共享前 32 字节，bg_checksum 都在 0x1e；修复了 32 字节版本的错误偏移和打包。CRC16 跳过校验字段，CRC32C 将其置零。
- 总空闲块高位位于超级块 0x158，且仅在 64bit 格式写入。删除了不存在的“空闲 inode 高位”写入，保留总块数/保留块数高位。

格式依据：[Linux ext4 块组描述符](https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html)及[超级块](https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html)。

初版检查器只检查 e2fsck 退出码，错误地将首次两次运行记为通过：实测`e2fsck -fn` 可在输出 `IGNORED`、错误计数和 `Fix? no` 时返回 0。现验收固定使用 C locale，要求完整的五阶段检查及该镜像的统计摘要，拒绝其他诊断。`tools/tests/test_multi_qemu_fsck.py` 覆盖这些零退出码错误、未知诊断、阶段缺失和错误镜像名称。保留原报告，不覆盖历史；证据审计另行标记它们不能通过。

## 复现

```sh
python3 tools/qemu/check-multi-qemu-kernel.py --workload linux-fs \
  --out .kernel-build/cluster/multi-qemu/fs-new --timeout 120

python3 tools/qemu/check-multi-qemu-kernel.py --workload linux-fs \
  --ext4-checksums metadata32 \
  --out .kernel-build/cluster/multi-qemu/fs-csum32-new --timeout 120

python3 tools/qemu/check-multi-qemu-kernel.py --workload linux-fs \
  --ext4-checksums metadata64 \
  --out .kernel-build/cluster/multi-qemu/fs-csum64-new --timeout 120

python3 tools/qemu/check-multi-qemu-kernel.py --workload linux-fs --mode smp \
  --out .kernel-build/cluster/multi-qemu/fs-smp-new --timeout 120

PYTHONDONTWRITEBYTECODE=1 python3 tools/tests/test_multi_qemu_fsck.py
```

默认 legacy 使用项目已有 mkfs feature 集，包含 uninit_bg/GDT_CSUM；另外两项分别显式启用 metadata_csum 并选择 32 或 64 字节描述符。均为无 journal 的新建 scratch 文件系统；没有更改现有用户磁盘，也没有减少目标 JVM 或 Minecraft 镜像的功能配置。测试覆盖这三种格式，不代表所有 ext4 feature 组合或大容量文件系统均已验证。

`.kernel-build/cluster/multi-qemu/` 下保留：

| 运行 | 审计结果 |
| --- | --- |
| owner-filesystem-01 | 文件正确，元数据不一致；旧检查器误报，不接受 |
| owner-filesystem-baseline-01 | 普通 SMP 同样复现；旧检查器误报，不接受 |
| owner-filesystem-02 | 修复后 legacy 双实例通过 |
| owner-filesystem-csum32-01 | metadata_csum、32 字节描述符双实例通过 |
| owner-filesystem-csum64-01 | metadata_csum、64 字节描述符双实例通过 |
| owner-filesystem-baseline-02 | 修复后普通 SMP 对照通过 |

`filesystem-owner-evidence.json` 重新校验日志、冻结 ELF、文件系统、提取结果与报告哈希，并用最终检查器重新审计所有 fsck 日志，区分旧报告的 passed 字段和实际接受结果。每次临时共享 RAM 都在对应进程停止后删除。

## 尚待完成

本阶段证明了跨实例磁盘程序加载和同步文件操作。真实 JVM、动态库加载、图形设备与 Minecraft 客户端仍需实际启动验证；物理 MCU 的共享 RAM/仲裁/缓存维护、异构部署和设备故障恢复仍保留原设计与验收要求。不能用本用例代替单个 Minecraft 跨 MCU/跨实例运行的最终验收。

后续真实 JVM 启动及同一 JVM 的跨实例 Java 线程执行已见[59](59-shared-jvm-execution.md)。Minecraft 图形与输入仍需继续验收。
