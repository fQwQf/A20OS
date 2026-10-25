# 集群部署与排障

本文是实际实现的操作入口。部署配置语法、监管状态格式见
[17-deployment-supervision.md](17-deployment-supervision.md)，ABI 见 01 文档。

可选的远程页服务独立配置、默认关闭；只读/可写模式、容量与流量限制、初始
文件和客户端 API 见 [63-configurable-remote-memory.md](63-configurable-remote-memory.md)。
它提供显式页快照和原子提交，不会自动给普通进程建立跨机共享地址空间。

## 1. 部署前提

每台机器有唯一持久 128 位 node_id，每次启动有非重复 boot epoch。
完整主机从显式持久配置恢复身份与种子，STM32 从 UID 和两页 Flash journal
生成身份/epoch。IP、UART 短地址、服务槽位都不代替持久身份。

| 角色 | 配置 role | 能力与职责 |
|---|---|---|
| Native 执行节点 | worker | RELIABLE；运行 agent、clusterd，可选 jobd |
| UDP/UART 网关 | relay | RELAY+RELIABLE；转发原来源事务，可同时执行任务 |
| 静态 MCU | 板级 leaf | LEAF+RELIABLE；预置算子、单缓存、专用 UART 周期泵 |

MCU 不加载 ELF、不运行文件配置监管者；它与完整主机使用相同 CL/CW 协议和
能力查询。不同资源档改变容量，不改变算子结果或事务语义。

## 2. 构建与启动

```sh
make ARCH=riscv64 native-cluster-agent
make ARCH=x86_64 CLUSTER_UART_COM2=1 dev-build
PATH=/path/to/arm-toolchain/bin:$PATH make ARCH=armv7m BOARD=stm32f103 \
  STM32_FLASH_KB=64 STM32_RAM_KB=20 STM32_CLUSTER_PORT=2 kernel-only
```

按实际机器替换配置中的 ID、种子地址和架构二进制后，持久保存配置。示例见
`docs/cluster/configs/worker-rv.conf` 与 `relay-x86_64.conf`。在目标 Native 环境：

```sh
/bin/cluster-supervisor-rv /bin/cluster.conf
/bin/cluster-wordcount-rv /tmp/input.txt
```

监管者预先验证配置与全部服务镜像，再设置不可变身份、安装种子、启动服务。
同一系统只能有一个 `cluster.supervisor`。每个子服务独立退避重启；监管者本身
由 init/外层管理器或操作者启动和恢复，仓库没有自动替换所有架构的 init 策略。
进程任务的可执行路径来自本地清单，不能通过网络传入任意路径或本机句柄。

STM32 控制台为 USART1，集群为 USART2（PA2/PA3）或 USART3（PB10/PB11）；
头节点 COM2 为 115200、8N1。双方使用原始二进制字节，不能启用终端换行转换。
具体固件参数、Flash 擦写与身份恢复纪律见 12、13 文档。升级固件不能擦除持久
epoch 区后继续使用原 node_id；维护时遵循 12 文档的重新配置要求。

业务单向接收需要在相关端点和中继升级后显式启用 `feature oneway`，
服务以 `A20_EXPORT_ONEWAY` 导出；MCU 则在首次握手前注册静态 sink。
默认配置不激活新增能力位。成功发送只保证本地入队，应用须观察自己的
业务完成条件后再关闭连接。完整契约见 [22-oneway.md](22-oneway.md)。

## 3. 拓扑与发现

只把实际邻居配置为 metric=0 种子。完整互联适合少量主机；树形拓扑适合串口
leaf 和网关。间接目的的路由由已确认邻居传播，不把所有节点伪装成同一个直连
链路地址。网关必须有 RELAY，双方都要有到原来源/目的的返回路径。

clusterd 先确认邻居，再传播有界成员页和租约路由；目录与能力仍通过真实
端点查询验证。显式路由优先于动态路由，租约清理只条件删除自己拥有的 tuple。
服务 any 只选择 ALIVE、租约有效且输入/算子能力匹配的实例；轮询不保证单个
作业均匀覆盖所有节点。来源表、服务表和路由表满时拒绝新增，不逐出仍在保护
的去重记录。MCU 最多 8 个间接来源、一个在途回复缓存。

完整 UDP 路径可按双方资源档分片；经过 UART 的路径退回 256 B 单帧，可靠
请求/回复业务上限分别为 206/202 B。切片还要减去 CW 头，并检查算子能力。
不要依据服务器的本地 RAM 向 MCU 发送超限消息。相关协商见 14–16、19 文档。

## 4. 排障顺序

| 现象/步骤 | 检查入口 | 判定与处理 |
|---|---|---|
| 本机服务未出现 | `cluster_snapshot(EXPORT)`、监管者本地状态 | 检查路径、启用项、退出原因与退避时间 |
| 远端连接失败 | `cluster_link_status` | DOWN 检查种子地址、串口引脚、握手双方 ID；SUSPECT 检查心跳进展 |
| 链路正常仍不可达 | `cluster_snapshot(ROUTE)` | 检查目标完整 ID、下一跳、RELAY、返回路由、动态租约 |
| 目录解析超时 | CD/1、CM MEMBER、CS 会话 | 检查最终 epoch、CS cookie 挑战、服务目录刷新；不能复用旧 boot 绑定 |
| 服务名找到但调用失败 | CW QUERY 和 Native 资源快照 | 比较输入/输出、算子 ID、budget、max_inflight 与路径 MTU |
| 大消息不进展 | `cluster_snapshot(kind=4)` | 检查分片资格、重组拒绝/超时、槽位和字节峰值 |
| 偶发缓存忙 | kind=3、重试/收据统计 | 查看未接纳重试与放弃收据；MCU 单缓存不得被强制逐出后重复执行 |
| 链路存在但丢帧增长 | `cluster_snapshot(kind=5)` | 分开查看 malformed、策略 drops 与重传；核对 CRC、身份/地址绑定和 TTL |
| 停止后资源仍增长 | kind=3 与 `/proc/a20/task_lifetime` | 分别检查代理/重试字节归零、任务和僵尸恢复；回复保留内存不等同泄漏 |

Native SDK 包装为 `a20_cluster_resources`、`a20_cluster_reassembly_resources`
和 `a20_cluster_link_resources`，计数口径与故障复现见
[20-hardening.md](20-hardening.md)。

性能测量与五分钟模糊档见 [21-performance.md](21-performance.md)；性能入口会
占用正在测量的镜像，运行期间不要重打包同一构建变体。
监管状态服务 LOCAL_ONLY；它返回统计，不能用于远程执行管理命令。当前没有
完整交互式 clusterctl，以上检查应通过 SDK 或已提供的验收程序执行。

## 5. 恢复与升级

agent 重启后服务 slot 更新，同一内核 epoch 不变；客户端重新解析服务。
clusterd 重启从内核快照恢复，已有业务连接继续工作。节点重启 epoch 改变，旧
绑定取消，必须重新确认和连接。jobd 根故障终止作业，没有持久作业日志；重复
CJ 提交会创建新作业。对不确定失败，只有明确 REPLAY_SAFE 的算子可以重派，
进程任务不能自动重派。关闭调用只是放弃结果，不承诺终止远端回调。

升级保留 wire v0 金样与 v1 协商兼容；新旧 CS 混合路径降级单帧。先验证独立
leaf/网关小拓扑，再逐批升级完整主机。不能用升级操作重置身份或清除持久 epoch
来绕过旧会话检查。无硬件实测时，固件成功链接不能代替 Flash、电气和持续运行
验收。

## 6. 验证与部署边界

```sh
make check-cluster-unit check-cluster-fuzz
make smoke-native-cluster smoke-native-cluster-supervisor
make check-cluster-reliable check-cluster-mesh
make check-cluster-hardening
python3 tools/check-cluster-gateway.py --gossip --abandon
make demo-wordcount
```

同一镜像变体的构建、打包和 QEMU 检查应顺序执行；运行中不能改写其基础镜像。
每个检查器自己的产物目录保留配置、日志及源文件哈希，证据索引见 08 文档。

CRC 检测损坏，邻接地址绑定与 CS 挑战抵抗旧会话重放；它们不提供密码学身份、
机密性或敌对网络完整性。当前信任边界为受控 LAN/专用串口。开放网络需要已
认证的外层通道或后续认证协议，不能把现有部署描述为自带加密的安全集群。
长期 soak、生产吞吐与物理板验收仍按 06 文档单独记录。
