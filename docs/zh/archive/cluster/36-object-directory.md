# 统一实例对象目录

## 机制与边界

实例的对象名由 instance UUID、domain UUID、内核 boot_epoch、对象序号和种类组成，布局沿用 [34](34-runtime-namespace.md)。本阶段将该名字接入实际查询：对象所属执行域保存带租约的目录记录，其他域通过相同 CO/1 协议查询。域是一个原生内核：共享 RAM 的多个 MCU 属于同一域；独立服务器内核各自属于一个域。对象表和编解码不含 STM32、ISA、地址宽度或传输驱动分支，也不分配堆内存。 32/64 位本地适配器保存自己的句柄，线上只传固定对象名。

这是按所属域分片的对象元数据目录。查询成功说明所属域此刻持有该身份记录，返回 LIVE 或 RETIRED；它不提供强一致全局枚举，也没有将远程对象变成本地 TASK 句柄。跨域 kill/wait、全局应用启动/退出归属、文件/显示/输入代理、持久启动代次和 Java 运行时仍须实现。Minecraft 验收继续依照[24](24-single-system-minecraft.md)。异构域可共享对象/服务协议，原生线程栈和 PC 的迁移仍受 [27](27-portable-domains.md) 的 ISA/ABI 相容性约束。

## 发布与生命周期

公共生产状态机位于 `kernel/include/uapi/a20/cluster_objects.h`，初始化传入已绑定的 runtime、固定容量数组、identify/release 回调和上下文。适配器负责串行调用，提供单调毫秒时间；publish/renew 的时钟溢出被拒绝。服务端可调整容量，协议不携带数组大小，也不要求 MCU 使用服务器容量。

Native 适配器 `user/svc/objectd.c` 是可选用户服务，32 条记录、8 个本地连接。它要求内核已 BOUND 且 CL node_id 与 domain_id 一致，导出 `instance.objects`。本地发布者经独立连接传入真实 TASK/THREAD/DEBUG 能力；目录调用公共 handle_identity 核验完整对象名，持有接收的引用。IPC 转移是共享引用，发布者仍持有原句柄。对象名本身不授权发布，也不能据此打开任意任务。

同一连接再次发布同一对象只续租并释放重复引用；另一连接发布该对象返回 CONFLICT。续租/撤回要求所属连接和 publication 序号都匹配。断开、到期和撤回均释放目录引用一次。目录引用保留任务身份，任务回收后查询为 RETIRED，不会强引用任务或阻碍 PID 复用。未注册、撤回或租约到期返回 NOT_FOUND；它们不等于任务死亡。失联/超时不推断 RETIRED，身份回调失败返回 FAILED。

publication 在一次目录进程内单调增加且不回绕；重新发布后的旧引用返回 EXPIRED。目录进程重启使旧导出槽位失效，旧通道不能绑定新槽位。内核重启则由 CD/2 runtime 启动代次和 CL 传输代次共同检查：旧对象名不能解析到新启动中的同序号任务。目录不是持久存储；重启后由存活发布者重新注册，不自动重放。

## CO/1 线格式

请求和回复都为 96 B，所有整数显式小端，UUID 为原始字节。可靠 UART 包含 CL/session/CRC 开销后为 150 B，在既有 256 B MTU 内。固定长度解码校验保留位、 op、cookie、状态、对象名和 publication；失败不发布部分输出。

| 偏移 | 请求 | 回复 |
|---|---|---|
| 0–3 | `CO`、版本 1、op | 原样协议头与 op |
| 4–7 | 租约毫秒；query/revoke 为 0 | status |
| 8–15 | 非零请求 cookie | 原样 cookie |
| 16–23 | publication；首次 publish 为 0 | 成功的 publication |
| 24–27 | 保留零 | 剩余租约毫秒 |
| 28–31 | 保留零 | LIVE=1 / RETIRED=2 |
| 32–95 | 64 B object_id | 原样 object_id |

op 为 QUERY=1、PUBLISH=2、RENEW=3、REVOKE=4。租约范围 1..30000 ms。 query 首次允许 publication=0，后续查询携带已获得的序号；renew/revoke 必须携带非零序号。失败和成功 revoke 的 publication/剩余租约/state 均为零。 status 为 OK=0、INVALID=1、NOT_FOUND=2、EXPIRED=3、DENIED=4、NO_SPACE=5、 CONFLICT=6、STALE=7、FAILED=8。STALE 表示对象所属实例/域/内核代次不匹配。网络通道只允许查询，发布能力不会经过网络序列化。所有附带引用在成功保存或拒绝时均被消费，畸形请求同样释放附带引用。

## 应用接口与部署

SDK `user/liba20rt/a20_objects.h` 提供 publish/renew/revoke/open/query/close。 publish 保持连接打开作为发布者身份。open 根据对象名判断本地/远程所属域，远程路径先通过 CD/2 检查实例和内核代次，再解析目录服务并核对传输代次，最后使用绑定通道查询；发现和查询共享一个有界预算，不降级到无实例身份的 CD/1 运行快照。每个引用只支持单个在途调用，错误后关闭通道隔离迟到回复。

```c
#include "liba20rt/a20_objects.h"
/* 内核已绑定，objectd 已启动，task 是当前持有的可转移任务句柄。 */
a20_object_publication_t pub;
if (a20_object_publish(task, 30000, 4000, &pub) == 0) {
    /* 将 pub.id 作为对象名交给同实例应用；定期续租。 */
    a20_object_renew(&pub, 30000, 4000);
    a20_object_revoke(&pub, 4000);
}
```

服务器在已有实例配置完成后显式启动 `/bin/objectd-<arch>`。该服务随 Native 镜像构建，但不加入默认启动项，也不改变现有 supervisor 的 CS/1 服务列表。调用者负责重启服务和重新发布。MCU 当前验证的是同一生产目录的进程内适配， identity/close 为真实 Native SVC；MCU 尚未实现 objectd 通道/网络服务。不能把这一验证写成物理 MCU 集群已可网络查询任务。

## 验收与普通使用成本

```sh
make check-cluster-unit
make check-object-directory-portability
make check-cluster-objects
CCACHE= QEMU_ACCEL=tcg,thread=multi python3 tools/native/check-native-sync.py --jobs 12
make CCACHE= PYTHON=<带 Unicorn/pyelftools 的 Python> check-mcu-program-rtl
```

跨架构宿主验收执行生产目录和显式身份/释放适配器，覆盖 x86/AArch64/RV64、 x86 ASan/UBSan，以及 ARMv7-M/RV32 freestanding 编译。这不替代用户态开机验收。实际三内核 RV64/AArch64/x86 流程检查真实能力发布、跨域 LIVE/RETIRED、撤回、到期、重新发布、目录服务重启、域内核重启和错误实例；宿主转发器保存实际 CD/1、CD/2、CO/1 报文，不能只依据测试程序 PASS 判定覆盖。双核 Native 同步应用和共享 MCU ELF/SVC/RTL 使用同一生产目录检查真实线程 LIVE→RETIRED→revoke，关闭次数恰为一，并继续原有线程/分配器/堆回收测试。

本阶段生产内核调度器、普通 syscall、网络快速路径和既有 ABI 均未修改。新增成本来自显式启动的 objectd、显式发布的引用和租约轮询；普通应用没有自动注册、目录遍历或额外系统调用。此边界不表示启用服务完全没有资源竞争，也不代替硬件性能测量。逐项运行结果、二进制和源文件摘要随提交归档在`.kernel-build/cluster/evidence/object-directory-<commit>/manifest.json`。

实际 ELF 启动、ISA 变体选择与按活动程序数分配见 [37](37-application-execution.md)。
