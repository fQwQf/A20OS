# 启动日志与故障取证

日志用于确定执行到了哪一层。它的可靠性仍取决于实际输出设备和存储链路，不能用“有日志功能”
代替故障时的成功落盘证据。PC 驱动复用与退出契约见[硬件复用指南](../drivers/guide/x86-hardware-reuse.md)。

## 实现边界

| 层 | 所属代码 | 位置及覆盖范围 |
|---|---|---|
| Loader | `kernel/arch/*/boot/uefi/` | ESP 的 `A20OS/boot.log`；可用日志分区上的 `early-<bootcount>.jsonl`；记录内核接管前的阶段 |
| 内核记录 | `kernel/core/klog.c` | 有界结构化内存环；IRQ 只记录，不直接进行文件 I/O；串口、framebuffer 是观察出口 |
| 持久写入 | `kernel/core/klog_disk.c` | 进程上下文排出 JSONL；通过 VFS 持有打开文件；不在设备驱动中实现 FAT 写盘 |
| 存储选择 | `kernel/fs/mount_setup.c` | 根据日志策略、GPT 分区和块设备来源选择目标；USB 与内置磁盘明确区分 |
| 桌面会话 | `packages/overlay/xfce/` | 原版 OpenRC 服务监督，保存桌面、Wi-Fi、触控板及电源服务日志 |
| 宿主读取 | `tools/a20log` | 读取已挂载目录或原始镜像，不启动目标系统 |

framebuffer 渲染属于 `kernel/drivers/char/fbcon.c`，映射和缓存属性属于架构层；
DRM 交还显示后恢复文本控制台，让关机同步进度可见。会话工具通过
`RAMFS_DESKTOP_TOOLS` 打包，与无线驱动开关独立。

## 目标选择与镜像布局

实例的 `[log]` 描述构建缺省值：

```toml
[log]
level = "info"
persistent = true
mode = "auto"
partition_mb = 64
max_bytes = 8388608
session = true
loader = true
```

- `auto`：优先启动发行版所在磁盘的 `A20OS log` 分区；没有选择结果时尝试内置 ESP，
  然后尝试可写根文件系统。歧义和错误会打印具体诊断。
- `partition`：选择专用日志分区；没有分区时报告失败。
- `esp`：只使用内置磁盘的 ESP 中 `A20LOG/`，不会把 USB 的 ESP 当作内置存储。
  不存在可写的内置 ESP 时报告失败，不静默改写另一块磁盘。
- `rootfs`：绕过专用日志分区，使用可写文件系统。
- `ram`：写入 RAM 路径，断电后不可保留。

UEFI 打包脚本生成 ESP、可选的 `A20OS distro` ext4 world 和可选的 `A20OS log`
FAT32 分区；无 world 的 shell 镜像省略发行版分区。分区顺序随新打包或历史升级布局而异，
开发工具按 GPT 类型/名称和 manifest 查找，不能假定日志永远是第2或第3分区。
发行版镜像使用启用 checksum-v3 的内部 JBD2 journal；日志分区仍独立于它。

内核会打印实际目标，例如：

```text
[LOG] persistent log at /var/log/a20-esp/A20LOG (internal ESP disk 0)
```

Lenovo 第18轮使用内置 ESP。其宿主路径通常为 `/boot/efi/A20LOG`；USB 日志分区为空
不能据此判定没有内核日志。`tools/a20 show <instance>` 可查看实例策略；临时构建可指定
`LOG_MODE=esp`，单次启动可使用 `a20.log.mode=esp`。级别、控制台、持久化开关和大小
也有 `a20.log.*` 调试参数，解析代码及边界以 `kernel/core/klog.c` 为准。

## 读取与判读

```sh
tools/a20log list /boot/efi/A20LOG
tools/a20log summary /boot/efi/A20LOG
tools/a20log show /boot/efi/A20LOG --level warn
tools/a20log list --image build/x86_64-pc/a20os-laptop-uefi.img
tools/a20log bootlog --image build/x86_64-pc/a20os-laptop-uefi.img
```

受权限保护的 ESP 需要宿主管理员读取。采集先复制日志和设备标识；读取异常 ext4 应使用
`ro,noload`，避免宿主挂载时回放日志改变证据。需要修复时先做独立备份，在副本验证，
写回脚本应核对设备身份、原始内容、修复范围和完整回读；开发验证不得默认写宿主系统盘。

`CURRENT` 是固定长度的 `key=value` 文件，记录 boot ID、loader bootcount、最后序号、
字节数及状态；内核记录是 `boot-<id>.jsonl`，应检查每一行可解析、序号连续及 boot ID 一致。
`booting`、`ready` 是执行阶段，`clean` 表示执行了关闭标记，不证明最终固件电源动作成功，
也不替代退出后的 fsck。断电的残缺尾行可以丢弃，丢失的完整记录必须如实报告。

分析慢关机时分开计算 GUI 请求、服务退出、最终 sync、各文件系统同步、JBD2 ordered/log/
checkpoint/finish 和固件动作。第18轮虽为 clean，最终同步仍105332 ms；这是待优化的实机
性能限制，不能拿 QEMU 的约10 s电源流程声称已解决。归档应同时标明内核版本、EFI 哈希、
根分区哈希、测试动作和采集方式，区分通过、失败及未测试。

## 开发验证与局限

```sh
python3 -m unittest tools.tests.test_a20log tools.tests.test_laptop_recovery \
  tools.tests.test_desktop_session_log
make smoke-laptop-esp-log
```

host 测试验证读取器及生产边界条件。ESP 门禁使用虚拟磁盘验证落盘与突然退出后的日志读取；
完整桌面门禁还应检查正常电源动作和退出后的文件系统。只有真实机器日志才能验收其存储链路。

块驱动不可用、挂载之前的崩溃、正在进行的未提交写入、被覆盖或写满的日志都可能丢证据。
Loader 文件只能覆盖固件阶段；早期内核仍需要屏幕或串口。日志写入失败后保留内存及控制台
诊断，不宣称具有独立于内核的故障存储能力。私有日志和原始磁盘副本保存在被忽略的 evidence
目录，提交代码时不纳入用户网络、进程和机器的原始数据。
