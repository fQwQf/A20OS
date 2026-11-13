# 原版 init 的内核接口

桌面使用 Alpine v3.23 的 `openrc=0.63-r1` 和 `openrc-init=0.63-r1`，
保留上游 PID 1、`openrc-run`、`supervise-daemon`、`openrc-shutdown` 二进制。
本地只提供平台准备、服务命令和桌面 D-Bus 适配；PID 1、依赖排序、监督及最终
TERM/KILL/电源调用由 OpenRC 执行。接口缺失应在内核实现，不能通过让上游程序
绕过该接口来完成验收。

## PID 1

PID 分配器初始化时保留 1，普通内核线程及用户进程不能分到它，包括分配器回绕。
初始用户镜像通过 `PROC_USER_IMAGE_BOOT_INIT` 单次领取 1，完成 auxv、地址空间和
进程组/会话初始化后才发布为 runnable。引导内核线程随后退出，用户态 PID 1
承担子进程回收。这个保留适用于 C 和 Rust PID 后端。

Lenovo 第 16 轮日志直接记录了 `target=1 name=i2c-hid signal=15`。此前 PID 1
被早期硬件线程占用，BusyBox 的普通重启请求会发给该线程。不能把这个现象仅归因于
用户态 init，也不能用 `reboot -f` 证明普通 init 重启路径正常。

## 命名管道

x86_64 旧式 `mknod`（133）必须补入 `AT_FDCWD` 后交给 `mknodat`（259）。
`mknodat` 创建 inode 元数据，不通过打开端点实现创建；因此 `mkfifo` 不会等待对端，
也不要求创建者同时有读写权限。ramfs/tmpfs 和 ext4 保存 FIFO 类型、权限及目录项。
不支持 FIFO 的文件系统返回不支持错误。

活跃 FIFO 以 mount/inode 标识共享一个内核管道对象，复用匿名管道的有界环形缓冲、
`PIPE_BUF` 原子写入、信号和等待队列。硬链接共享流；取消链接后，仍打开的流继续存在；
同名新 inode 使用独立流。最后一个打开文件描述对象关闭后释放缓冲，下一次打开得到空流。

阻塞只读/只写打开等待对端；非阻塞只写且没有读者返回 `ENXIO`；`O_RDWR` 遵循 Linux
行为，无需对端即可打开。打开代数避免对端在等待者重新调度前关闭导致丢失 rendezvous。
`O_PATH` 不计入读写端点。支持 `poll`/`epoll`、`FIONREAD`、EOF、`POLLHUP`、`POLLERR`
及断开写入的 `SIGPIPE`/`EPIPE`。FIFO 不可 seek、mmap 或 fsync。

FIFO 创建同时检查规范化父目录上的 Landlock `MAKE_FIFO` 权限；符号链接别名不能
越过允许的目录。`open(O_CREAT)` 屏蔽文件类型位，只创建普通文件，避免绕过此检查。

文件系统目录操作保留 namespace gate；FIFO 打开等待在 gate 释放后进行，数据读写只访问
内存管道，不持有文件系统 I/O gate、不增长 inode 或消耗磁盘配额。ext4 删除无磁盘负载的
FIFO 时不解析 extent tree，避免把合法特殊 inode 误判为损坏并 poison journal。
关闭通知期间仍保留管道引用，防止并发关闭释放另一端尚在通知的对象。

语义依据：[Linux fifo(7)](https://man7.org/linux/man-pages/man7/fifo.7.html)、
[open(2)](https://man7.org/linux/man-pages/man2/open.2.html)。这里没有宣称整个 Linux
文件系统 ABI 的所有标志和特殊设备节点均已实现。

## 初始环境与关机广播

OpenRC 会用不同环境执行两次 `md5sum /proc/self/environ`，检查 procfs 是否工作。
空占位文件会使它误判挂载失效。exec 现在记录初始环境在新 MM 中的地址范围；fork 继承
地址范围，procfs 在权限检查后持有 MM 和所读物理页的引用，读取实际用户内存。
环境按 NUL 分隔，读取容量可增长，`stat` 大小为 0。它不是当前 libc 环境字典的序列化。
参考：[proc_pid_environ(5)](https://man7.org/linux/man-pages/man5/proc_pid_environ.5.html)。

`kill(-1, sig)` 按进程代表发送一次，排除调用者的线程组、PID 1 和没有用户 MM 的内核线程。
旧实现逐 TID 重复向共享 pending 队列添加信号，在其他 CPU 已消费前一个信号时可能产生
重复递送。这个语义修复有生产函数主机回归和旧实现负对照；不应将它等同于已证明某个
用户态退出崩溃的唯一原因。

## 挂载属性与关机反馈

2026-11-13 第17轮实机确认：换用 OpenRC 后桌面、网络可用，但图形重启至最终
reboot 系统调用入口仍需约101 s，主要等待在服务停止后的最终同步段，尚未进入固件。
这不是成熟 init 需要固定等待一分钟。

内核曾将 `VFS_MOUNT_DETACHED=0x2` 混在 Linux `MS_*` 属性中；`MS_NOSUID` 也是2。
OpenRC 更新 `/run` 属性后，查找跳过其 tmpfs，运行状态和 socket 标记被写进底层 ext4。
下轮启动便可见 D-Bus 的 Address in use。现在 mount 对象单独保存 `detached`，
挂载属性更新也保留共享命名空间的所有权标记。真正 detach 仍不可从普通路径到达。
这项修复不代表已经补齐全部 Linux 挂载属性的安全策略。

连续 JBD2 payload 以最多128 KiB合并写出，遇到extent或环边界拆分，内存不足退回单块。
原有 ordered data、descriptor/data flush、commit、checkpoint 及清除日志的屏障保持原样。
系统 sync 与 JBD2 各阶段现在有耗时记录；不以省略同步或强制复位缩短关机。
图形移植工作区还恢复了桌面释放 DRM 后的 framebuffer 文本控制台，避免留下静止旧桌面。

同一候选在16 CPU/8 GiB QEMU中通过USB图形关机、USB普通reboot、NVMe图形重启，
每轮留下64个文件、16 MiB数据后走正常电源流程，均约10 s，退出后fsck=0、clean、
RECOVER=0、last_orphan=0。含控制台截图；实机优化耗时仍待第18轮验证。

## 验证

```sh
python3 -m tools.tests.test_boot_init_pid
python3 -m tools.tests.test_kill_broadcast
python3 tools/native/check-native-task-object.py
python3 tools/tests/test_session_power.py
python3 -m unittest tools.tests.test_mount_flag_namespace tools.tests.test_jbd2_payload_batch
# 以下三个脚本在 A20OS 测试客体中运行；前两个也可在 Linux 上作对照。
python3 tools/tests/fifo_guest.py
python3 tools/tests/proc_environ_guest.py
python3 tools/tests/openrc_supervision_guest.py
```

FIFO 客体测试覆盖两种文件系统、两套 x86_64 系统调用入口、打开/关闭竞争、信号中断、
原子写入、多写者、硬链接与路径复用。环境测试覆盖独立 exec、16 KiB 环境、原始用户页
修改和 fork。OpenRC 测试用临时服务验证启动、子进程异常退出后重新拉起、状态及通过
FIFO 停止，退出后删除测试服务。

验收必须运行普通命令行重启、桌面 D-Bus 关机/重启，并在 QEMU 退出后对实际写过的
ext4 副本执行只读 fsck，检查 clean、RECOVER=0、last_orphan=0。具体镜像哈希、运行日志
和实机安装状态见 Lenovo 移植交接和对应 evidence；QEMU 通过不能代替实机复位验收。

上游流程：[OpenRC init guide](https://github.com/OpenRC/openrc/blob/0.63/init-guide.md)、
[OpenRC user guide](https://github.com/OpenRC/openrc/blob/0.63/user-guide.md)。
