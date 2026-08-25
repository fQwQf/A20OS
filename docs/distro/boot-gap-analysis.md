# 发行版启动差距分析：从无法运行到 XFCE 桌面稳定运行（2026-08-26）

本文档记录一次针对"宣称可运行 stock Alpine + XFCE Wayland，但实际不能"的
系统性排查：在 QEMU riscv64 上以 `make distro-run` 等价配置（fat32 + ext4
rootfs、virtio-gpu/keyboard/mouse、headless serial）复现失败，逐个根因修复，
并在干净镜像上完成两轮 10 分钟稳定性浸泡。所有内核修复位于分支
`fix/distro-boot-gaps`（worktree `~/OS/A20OS-bootfix`）。

## 复现方法

```sh
# 内核（worktree）
make ARCH=riscv64 BOARD=qemu-virt-riscv64 dev-build PYTHON=python3
# Alpine rootfs（约 2 分钟，a20rootfs-builder 容器）
docker run --rm -v "$PWD:/w" -w /w a20rootfs-builder:latest \
  sh -c 'ARCH=riscv64 OUTPUT=build/alpine/rootfs.img ROOTFS_SIZE_MB=8192 \
         bash user/rootfs/alpine/build.sh'
# headless 启动（-snapshot 保证每轮镜像状态一致）
qemu-system-riscv64 -machine virt -bios default \
  -global virtio-mmio.force-legacy=false -m 1G -display none -smp 1 \
  -serial file:boot.log -monitor none -snapshot \
  -drive file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,...,id=x0 \
  -device virtio-blk-device,bus=virtio-mmio-bus.0,drive=x0 \
  -drive file=build/alpine/rootfs.img,...,id=x1 \
  -device virtio-blk-device,bus=virtio-mmio-bus.1,drive=x1 \
  -device virtio-keyboard-device,bus=virtio-mmio-bus.5 \
  -device virtio-mouse-device,bus=virtio-mmio-bus.6 \
  -device virtio-gpu-device,bus=virtio-mmio-bus.7 \
  -kernel .kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf
```

注意两个环境陷阱：

- **QEMU 以 RW 打开 rootfs.img**：一轮崩溃的 boot 会污染镜像（半创建的
  目录等），下一轮的"失败"可能是上一轮的遗产。测试必须 `-snapshot` 或
  每轮重建镜像。
- **宿主机负载**：TCG 对宿主 CPU 竞争极其敏感；本轮排查中失控的索引进程
  （合计 ~570% CPU）曾让 boot 时间翻倍并伪装成"卡死"。

## 已修复的内核差距（按致命程度排序）

### 1. inotify 读路径自死锁 —— 整个桌面被楔死（已修复，commit a94a3d2bd）

`inotify_ops_read()` 的等待循环有三条退出路径，其中两条（入口快路径：
事件已在队列；唤醒路径：park 醒来后重新拿锁再判断条件）在退出时**仍持有
`inst->lock`**，而随后的搬运会循环无条件再次 `spin_lock(&inst->lock)`。
自旋锁不可重入 → 同 task 自旋死锁。真实桌面上 GLib GIO 文件监视器
（xfce4-panel / thunar / xfconfd）首次读 inotify fd 即触发：

```
[LOCK-STALL] cpu=0 lock=... waiter=63 owner=63 owner_ra=inotify_ops_read+0x17e
（此后 287 行串口日志全是 LOCK-STALL，桌面再无任何进展）
```

修法：把"准备 park 期间数据到达"并入 link 判定，循环出口统一释放一次锁，
搬运循环保持每次 pop 拿/放锁。已知教训（known-issues.md 第二节）是 mutex
加了同 task 递归，但 spin 锁没有也不应有递归——这类"多退出路径 + 出口再
加锁"的结构就是下一个 LOCK-STALL 的模板。

### 2. socket 把协议类型当文件状态标志安装 —— GLib abort 连环杀服务（已修复，
commit ae4e4ee0a)

`net_socket_create()` 用 `net_socket_install_file(s, type)` 把 **socket
type** 直接写进 vfile flags。F_GETFL 会把它原样返回给用户态：
SOCK_STREAM(1) 被读成 O_WRONLY；AF_NETLINK 的 SOCK_RAW(3) 读出访问模式 3
——GLib 的 `g_io_unix_get_flags()` 对 `fl & O_ACCMODE == 3` 直接
`g_assert_not_reached()` abort。会话服务（xfsettingsd 等）随机被杀，
Xfconf 反复重激活。Linux 语义：socket 永远可读写。修法：以
`O_RDWR | (SOCK_NONBLOCK ? O_NONBLOCK : 0)` 安装，与 accept()/AF_ALG
路径一致。

对照 na-kernel：其在 vfs_file.c 明文注释 "O_CLOEXEC … must never leak
through F_GETFL or SCM_RIGHTS into the shared open-file-description
status flags"，并把创建标志/状态标志分层管理——A20OS 的 vfs_fcntl(F_SETFL)
虽有 accmode 掩码保护，但 socket 创建路径绕过了这层防线。

### 3. 僵尸线程组 leader 被提前回收 —— 内核 panic（已修复，commit 131b6dcbf）

leader 是 tg_next 链的锚点（它自身没有 tg_prev_ptr，tg_unlink 对它是
no-op）。三个回收路径（sched_reap_zombies / proc_wait4 /
proc_reparent_children）都不检查"组内是否还有活线程"，一旦 leader 僵尸
被回收而成员还活着，成员的下一次组遍历（proc_find_live_thread_reaper_locked
等）就在已释放内存上行走。桌面场景触发器：dbus-daemon 对激活超时的服务
（tumblerd）SIGKILL，多线程 glib 进程拆除期间命中：

```
KERNEL PAGE FAULT ... pc=proc_find_live_thread_reaper_locked+0x62
KERNEL PANIC → firmware poweroff
```

间歇性（clean1 必现、clean2 未现），与 SIGKILL 时线程退出进度竞态有关。
修法：新增 `proc_tg_group_dead_locked()`，三处销毁闸门统一加守卫——leader
僵尸只在全部活成员退出后可回收，与 Linux "leader 存活至组死亡" 语义一致。

## 新增的 ABI 能力

### SO_PEERPIDFD（commit 312a1ac7a，对齐 na-kernel）

na-kernel 的 unix socket 支持 `SO_PEERPIDFD`（Linux 6.5 UAPI），dbus 这类
守护进程优先用它做 peer 身份识别。A20OS 已有 pidfd anonfd 底座，补上该
getsockopt 即可；无 peer cred 时返回 -ENOTCONN，与 Linux 一致。

## 遗留问题（未在本轮修复，按影响排序）

1. **tumblerd 启动即静默挂起**（确定性）：dbus 激活 120s 超时，缩略图功能
   不可用。实测在真实 session bus + 正确 env 下同样无任何输出地挂住，
   怀疑点在 dlopen/GIO module 扫描或某个阻塞 syscall；需要 per-process
   syscall 追踪设施才能进一步定位（A20OS 目前没有 strace 等价的观测手段
   —— 这本身就是一个值得补的工具差距）。影响：一条 dbus 报错，桌面不受阻。
2. **dbus 日志 peer pid=-1**（外观）：Alpine dbus 1.16 从 SCM_CREDENTIALS/
   pidfd 族获取 peer 身份，A20OS 的 SO_PEERCRED 数据面在 accepted 连接上
   是否始终填充、SCM_CREDENTIALS 捕获时序等还需核对。uid/policy 不受影响。
3. **udevd worker 处理 uevent 全部超时被杀**：eudev 规则引擎在 A20OS 上
   "能用但不完整"（known-issues.md 第三节遗留），worker 卡在某个未知
   syscall。输入设备靠 seatd 直连路径兜底才工作。
4. **xfdesktop 图标 CRITICAL**（一次性）：`G_IS_FILE_INFO` 断言失败，桌面
   图标部分缺失，非致命。
5. **elogind 无 cgroup**：挂载 /sys/fs/cgroup 失败（内核无 cgroup 子树），
   elogind 降级运行，文档已有记录。

## 结论

修复 #1–#3 后，干净镜像上的 stock Alpine rootfs 可以稳定引导到 XFCE
Wayland 桌面：labwc/wlroots 完成 1024x768 modeset、面板 layer surface
持续渲染、Xfconf/dbus 服务正常激活、键鼠可用；两轮 600s 浸泡零 panic、
零 LOCK-STALL、零 GLib abort。README 所述能力在这些修复之后才真正成立；
剩余差距集中在缩略图守护进程挂起、udev 规则引擎完整性与 peer 凭证数据面
三处，均有明确的下一步定位手段。
