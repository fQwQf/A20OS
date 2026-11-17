# 持久部署配置与服务监管

`user/svc/cluster_supervisor.c` 读取本地配置后，设置内核身份和种子路由，监管 agent、clusterd、jobd。共享解析器及退避规则在`kernel/include/uapi/a20/cluster_config.h`，无系统调用、堆或全局状态；MCU 可按预算编译缩小种子数组，主机负责进程监管，静态叶子继续使用固定服务周期泵。统一的是节点、能力、传输、服务和工作请求；固件不需要模拟主机进程。

## 1. 配置与启动

将 [worker-rv.conf](../../../archive/cluster/configs/worker-rv.conf) 或[relay-x86_64.conf](../../../archive/cluster/configs/relay-x86_64.conf) 按部署修改后保存到节点持久磁盘：

```sh
/bin/cluster-supervisor-rv /bin/etc/a20/cluster.conf
```

配置路径是显式参数，不依赖当前目录或环境变量。FAT 镜像根通常挂载在 `/bin`，外部 rootfs 可以使用 `/etc/a20/cluster.conf`。替换为对应架构的 Native 后缀。启动本监管进程仍由启动脚本、系统服务管理器或操作者负责；它不自动修改系统 init，也不把示例身份作为默认部署配置。系统重启后恢复持久部署配置；若启用公共实例绑定，启动 epoch 必须由部署层更新，不能照搬上次启动的 epoch，见[34](34-runtime-namespace.md)。

格式为逐行 ASCII，支持空白、CRLF 和以独立 `#` token 开始的行尾注释，拒绝未知字段、重复字段、额外 token、NUL、非法 ID 和超长行。没有 shell 展开或远端路径执行。前三项必填各一次，其余可选：

| 字段 | 含义 |
|---|---|
| `version 1` | 配置版本 |
| `node <32hex>` | 持久完整节点 ID；拒绝全 0/全 ff |
| `role worker/relay/leaf` | 分别对应可靠工作节点、可靠转发节点、可靠叶子能力 |
| `instance <32hex>`、`epoch <非零十进制uint64>` | 可选且必须成对；绑定公共运行实例，域 ID 使用 node，见 [34](34-runtime-namespace.md) |
| `feature oneway` | 可选，至多一次；启用可靠业务 SEND，默认关闭；与 role 行顺序无关 |
| `seed <32hex> udp:<IPv4>:<port>` | 可选显式直连种子；拒绝自身、重复节点和非法地址 |
| `seed <32hex> uart:1` | 可选 UART 邻居 |
| `service agent/control/jobs <绝对路径>` | 可选的三个固定职责，各至多一个本地可执行文件 |
| `process <绝对路径>` | 可选 agent 进程任务 ID 1；要求配置 agent |

默认上限：4096 B 文件、191 B 行、16 个种子、127 B 路径、3 个服务。配置解析只在全部有效后发布结果，失败不改变输出记录。监管进程先解析完整文件并打开所有服务镜像，再改变身份或路由。已设置的内核身份和能力必须一致；不能借配置重载改变当前启动身份。种子只 ADD：已有完全相同的静态路由可以复用，冲突配置失败，不覆盖用户路由；中途失败条件删除本次已添加的路由。身份一旦成功设置即保持至本次 OS 启动结束，不支持回滚为未配置身份。

运行期不热重载；修改持久配置后正常停止并重新启动监管进程。已有内核路由保持有效，减少种子清单不会自动删掉此前的显式路由，需明确 DEL；变更身份或角色需要重启 OS。MCU 生产身份仍来自 UID 与两页持久 epoch journal，见[12-mcu-identity.md](12-mcu-identity.md)，不要求 MCU 有文件系统。

## 2. 生命周期、失败与资源

本地 `cluster.supervisor` 以 LOCAL_ONLY 导出且不使用 REPLACE，保证一个活跃监管者；重复启动失败，不接管或结束原服务。服务镜像句柄保持打开供重启复用。每个服务独立非阻塞轮询 task_wait，只有确认旧子进程已退出并回收后才启动新实例。失败退避为 100、200、400…6400 ms，最多 6400 ms；持续运行 60 s 后失败档位重置。计数或时钟加法溢出停止该项重启，不能回绕为无限快重启。启动成功仅表示进程存在；服务可用性仍由 CD/CW 和 clusterd 探测确认。当前不对永不退出但失去进展的进程主动做健康重启。

SIGTERM 正常停止：通知所有子进程、持续回收；1 s 后强杀仍存活的项，2 s 内结束清理。正常退出不删除身份或种子。监管进程被 SIGKILL 时，新的 Native `A20_SPAWN_PARENT_BOUND` 标记在子进程可运行前设置父死亡 SIGKILL，确保旧服务不成为无人监管的实例。agent 启动的进程任务也使用该标记。此标记不是跨 jobd 作业恢复；协调者死亡仍终止作业。

Native `task_spawn` 结构和 syscall 号保持不变，flags bit0 为 PARENT_BOUND，未知位和 v2 非零 reserved 返回 INVALID_ARGUMENT。默认 flags=0 保留普通子进程生命周期。使用已有 pdeathsig 的受锁退出路径，不增加无主后台清理线程。监管者本身需要由外层 init/服务管理器重启，或由操作者重新启动。

真实重启测试还修复了 Native 可执行文件引用：临时 fd 安装会消耗 lookup 引用， close 已释放它，不能再额外释放。修复后一个预打开镜像可反复 spawn，句柄在正常结束时准确释放，不再出现 `vnode_put on freed vnode`。

## 3. 只读本地状态

通过 LOCAL node（全 0 ID）连接 `cluster.supervisor`。请求 8 B：`CS`、版本 1、其余零；回复 128 B：同魔数版本，offset4 为 3，offset8 起为 agent/control/jobs 三行，每行 40 B：

| 行内偏移 | 内容 |
|---|---|
| 0/4 | flags（1 配置启用、2 进程存在、4 重启耗尽）、pid |
| 8/16/24 | 启动尝试次数、退出/启动失败次数、下一启动单调时刻 ms（u64） |
| 32/36 | 最近 exit_code 或启动 errno、exit_reason |

它是独立本地状态协议，与 CL 的 CS 端点挑战没有共享远端入口。没有通过该接口发送配置、路径、kill 或远程管理命令。状态中没有可转移的任务或文件句柄。

## 4. 验证

```sh
make smoke-native-cluster-supervisor
python3 tools/check-cluster-unit.py
make check-cluster-fuzz
```

真实 Native smoke 从磁盘文件读取配置，拒绝非法配置且验证身份未被改变，启动三项服务并实际解析/调用 DOT，拒绝重复监管者，反复重启立即退出的测试镜像，正常停止，强杀监管者后确认子服务导出全部消失，再从相同持久配置恢复调用。检查最终导出/代理/在途归零，任务/僵尸恢复基线、生命周期错误不增长，禁止 VFS BUG、PANIC 和 SIGSEGV。日志在`.kernel-build/smoke/native-cluster-supervisor-riscv64.log`。共享解析器额外运行 10 万次截断/变异 ASan/UBSan 检查；退避上限、重置和计数溢出均有确定性测试。这些是主机/QEMU 证据，没有物理 MCU 文件配置运行证据。

慢速执行环境可用 `timing-scale 1..16` 成比例放宽控制面期限与租约，默认 1 保持既有行为；协议、取舍和跨架构验收见 [35](35-instance-membership.md)。
