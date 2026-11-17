# 统一应用入口执行 Linux ABI 程序

`appd`、`appctl-run` 和 CA/1、CA/2 的启动、负载选择、查询、停止、回收流程现在也能管理 A20OS 上的 Linux ABI 程序。新增 Native `app-exec` 先作为普通根任务启动，再调用已有 `execve` 替换自身镜像；owner 持有的任务引用和`object_id` 保持。没有创建另一套按板型区分的集群协议。

## 能力与部署

`abi_info.feature_bits` 和 `feature_test` 的索引 4 定义为`A20_ABI_FEATURE_LINUX_EXEC`，仅 BOTH 构建公布。它不占用`execution_info` 的特性位，也不改变其 64 字节布局或线上兼容掩码。纯 Native 和 MCU profile 不因此获得 Linux 装载器。

清单增加可选的 `require-feature N`，N 为 0–255 的十进制 ABI 能力索引，可配置多个不同索引，重复或非法值使清单解析失败。appd 启动时仅注册本机满足全部要求的变体；无此行的旧清单行为保持。全部变体被过滤时 appd 返回 3，不导出服务；部署时应给该节点保留可执行变体。能力查询失败也会过滤需要能力的变体。旧版 appd 不理解新行，部署此清单需更新 appd。

例如在已按 [35](35-instance-membership.md) 绑定实例的 x86_64 图形会话中：

```text
A20APPS1
app cd000000000000000000000000000001 /usr/local/bin/app-exec-x86_64 62 64 1 1 2
require-feature 4
arg /bin/sh
arg /usr/local/bin/minecraft
arg --demo
arg --width
arg 854
arg --height
arg 480
env HOME=/root
env PATH=/usr/local/bin:/usr/bin:/bin
env WAYLAND_DISPLAY=wayland-0
env XDG_RUNTIME_DIR=/run/user/0
env GAMEDIR=/root/.minecraft-cluster-owner
end
```

WAYLAND_DISPLAY 和 XDG_RUNTIME_DIR 必须替换为目标节点实际图形会话的值。同一 UUID 可配置 ARM64、RV64 的 app-exec 变体，沿用各自 ELF machine 和调用 ABI。app-exec 的构建和输出路径与其他 Native 服务一致。

```sh
/usr/local/bin/appd-x86_64 --manifest /etc/apps.conf
# 在另一个已绑定同实例的调用任务中运行；域 UUID 按实际部署替换。
/usr/local/bin/appctl-run-x86_64 cd000000000000000000000000000001 20261013 30000 101112131415161718191a1b1c1d1e1f
```

app-exec 要求绝对程序路径，并原样传递其余 argv 和清单 envp。脚本可显式用`/bin/sh /path/to/script`；若脚本最终 `exec java`，JVM 保持同一个根任务。命令行新增一次 `APPCTL: root=<domain>/<serial>` 输出，便于对应真实任务。

Linux 文件描述符表由启动 appd 的 Linux 会话继承；本次没有实现任意 Native stdio 句柄到 Linux fd 的转换。图形、Java、动态库和资源仍须部署在实际运行节点。输出使用该节点的终端，未新增远程图像、音频或输出代理。

## 生命周期

| 结果 | 含义 |
|---|---|
| RUNNING | 已发布 app-exec 根任务，不表示后续 JVM 或 GUI 已就绪 |
| EXITED 64 | app-exec 参数错误 |
| EXITED 126 | 执行域未提供 Linux exec 能力（正常能力筛选会提前排除） |
| EXITED 127 | wrapper 已启动，但替换程序失败，例如路径不存在 |
| EXITED 128+signal | 替换后的 Linux 程序被信号终止 |
| EXITED 其他值 | 实际程序的退出码 |

Native task_kill 在 BOTH 构建中识别已转换到 Linux ABI 的任务，将信号交给 Linux 信号状态；致命进程信号结束整个线程组。task_wait 正确解码信号退出，不再把 SIGKILL 误报为 0。原 Native 分支保留。根任务保持与等待覆盖了实际多线程程序；没有增加后台后代树的通用托管语义。底层 exec 的低内存失败事务回滚未在本次验收中覆盖，不以 ENOENT 用例推断所有失败路径均安全。

## 已完成的执行验证

`application-exec/mixed-01/events.json` 为 PASS。真实 RV64、AArch64、x86_64 A20OS 内核通过同一 API 执行六个 musl Linux ELF，每个包含主线程和两个 pthread 工作线程；活动程序按 2/2/2 分配。每个程序校验空格、空字符串、 UTF-8 参数和环境，工作线程实际计算 1..1000 的和。查询确认 exec 前后根身份保持；停止后取得 137、活动数归零并回收记录。各域另有正常退出 41、缺失目标退出 127 和能力过滤用例。原 Native 程序、丢包、重启、CLI 和回收回归同时通过。此处均衡单位是活动程序数，不是 CPU 使用率。

`application-exec/client-01/report.json` 为 PASS：同一 appd/appctl 入口实际启动 Minecraft Java 1.21.11 demo、OpenJDK 21，显示欢迎页、主菜单和 Options， Tab/Enter/Escape 输入有效；Quit Game 后 owner 取得退出码 0。实际根身份为`101112131415161718191a1b1c1d1e1f/0000000000000004`。运行于 A20OS x86_64 四核 KVM、Wayland/virgl，只有一个候选域；不能用它证明单个 Minecraft 的线程已经跨物理 MCU 执行。音频使用 No Output，未验收世界、账号或联机。测试 VM 在应用正常退出后由主机 QMP quit 收尾。

```sh
make check-native-exec-features
make check-native-thread-admission
make check-cluster-unit
make check-cluster-apps
```

前两次 x86 三节点测试曾错误地期待 resume 检查已释放 ticket；实际 resume 只连接 owner，QUERY 才返回 EXPIRED。修正测试后混合架构通过，失败日志保留。首轮混合测试冻结的是新增 root 日志前的 CLI；客户端测试使用包含该日志的新 CLI。最终混合回归 `mixed-final/events.json` 同样 PASS，包含新 CLI 与测试连接释放修正。清单解析在三种服务器 ISA 实际执行、ASan/UBSan 检查，以及 ARMv7-M、RV32 freestanding 编译通过。重复运行测试会选择新的证据目录，保留已有结果。

## 复现文件与磁盘占用

记录在 `.kernel-build/cluster/application-exec/`，包含冻结内核/启动盘、报文、构建和测试日志。客户端使用 `-snapshot`；退出后丢弃 guest 磁盘写入，不声称保存了游戏世界。临时 world 在核对退出和 SHA-256 后移除，保留 46 个 4 KiB 替换块（184 KiB）、偏移和 SHA-256，可在原始 world 上重建启动输入：

- 基础 world：`19418c7d553b320a977542a9b215a35bf406aa07095dc6fe30458906b9c6b194`
- 准备后 world：`60d70873db5effc138ead43d2e9bf9bde7692f7c8c25e031d01a492b96c372f4`
- 客户端内核：`6678d9c941ddb57fc7f943696b1f4b2607908918e558753463de245b587a8a7b`

基础 world 已归档在 已删除的运行证据，补丁重建哈希验证通过，不再保存另一份完整 world。阶段源码、结果和文件哈希归档在 已删除的运行证据。

物理多 MCU 的最终目标仍见 [24](24-single-system-minecraft.md)。本次交付统一应用管理入口的真实 Linux 程序执行和主机客户端证据；MCU 上的完整 Java 客户端运行时、图形和至少两块物理板验收仍未完成。
