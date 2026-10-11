# 从 Native channel 到统一算子调用

本教程使用现有 `cluster-agent` 和共用 DOT 算子；完整实现位于
`user/svc/cluster_agent.c`、`user/liba20rt/a20_cluster_agent.h` 与
`kernel/include/uapi/a20/cluster_work.h`。部署身份、种子和服务启动先按 17 文档完成。

## 1. 服务导出

完整主机创建 channel pair，将一端以名称 `work` 导出；服务端通过该端接收
远程 FIFO 请求。Native 本地 connect 会注入一个独立连接句柄，SDK server pump
分别轮询它与远程 FIFO，避免回复交叉。生产 agent 已实现该生命周期：

```c
#include "liba20rt/a20_cluster_agent.h"
/* agent is a static-lifetime a20_work_agent_t with bounded callbacks. */
static int serve(const a20_work_agent_t *agent) {
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0)
        return -1;
    if (a20_cluster_export(pair.endpoints[0], "work", A20_EXPORT_REPLACE, 0) < 0) {
        a20_hdl_close(pair.endpoints[0]);
        a20_hdl_close(pair.endpoints[1]);
        return -1;
    }
    a20_hdl_close(pair.endpoints[1]);
    a20_cluster_agent_server_t server;
    a20_cluster_agent_init(&server, pair.endpoints[0], agent);
    while (!(a20_signal_check() & (1ULL << 15))) {
        if (a20_cluster_agent_pump(&server) < 0)
            return -1;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
    return 0;
}
```

该函数由进程入口调用，进程退出回收导出；不把栈上的回调表交给已返回的函数。
所有权与替换语义见 01、03 文档。当前远程导出要求按接收顺序回复；任意并行
乱序完成需要额外事务服务接口，不在此 FIFO 契约内。

MCU 不创建 Native channel，而是在握手前调用 `a20_mcu_leaf_set_agent()` 注册
静态表，随后周期泵调用同一 `a20_work_dispatch`。表重复 ID、超静态预算或进程
回调会拒绝。DOT、SUM64、词频与 CONCAT 已由默认注册表提供。

## 2. 能力选择和调用

下面的函数通过本机 clusterd 选择支持 DOT 且能接收 12 B 输入的存活实例，
绑定其实际服务 slot/boot epoch。它既能选中主机，也能选中 MCU：

```c
#include "liba20rt/a20_cluster_client.h"
static int remote_dot(uint64_t *sum) {
    a20_handle_t ep = A20_HANDLE_NULL;
    a20_cluster_selection_t target;
    if (!sum || a20_cluster_connect_uri("service://any/work", A20_WORK_OPERATOR,
            A20_WORK_DOT, 12, 1000, &ep, &target) < 0)
        return -1;
    uint8_t input[12], wire[44], reply[40];
    a20_work_put32(input, 1);
    a20_work_put32(input + 4, (uint32_t)-7);
    a20_work_put32(input + 8, 6);
    a20_work_request_t request = {.kind = A20_WORK_OPERATOR, .job = 1,
        .operator_id = A20_WORK_DOT, .budget_ms = 1000, .attempt = 1,
        .input = input, .input_len = sizeof(input)};
    int n = a20_work_encode(wire, sizeof(wire), &request);
    uint32_t got = sizeof(reply);
    int r = n < 0 ? -1 : a20_cluster_exchange(ep, wire, n, reply, &got, 1500);
    a20_hdl_close(ep);
    a20_work_result_t result;
    if (r < 0 || a20_work_result_decode(reply, got, &request, &result) < 0 ||
        result.status || result.output_len != 8)
        return -1;
    *sum = a20_work_u64(result.output);
    return 0;
}
```

结果为模 2^64 的 −42。真实客户端必须为独立作业生成合适的 job_id；常量 1
只用于此单次调用示例。错误的作业、分片、算子或旧 attempt 回复不能被接受。
CL 在同一事务重传时去重；应用重新调用创建新事务，不能借此推断跨重启
exactly-once。业务错误在 CW status，传输失败通过 channel 错误返回。

直接寻址可使用 `service://<32位十六进制node_id>/work`，由 CD 目录解析实际
槽位和 epoch；它不会替调用者自动证明某个算子兼容，需先执行 CW QUERY。
本机服务直接 `a20_cluster_connect(0, 0, "work", ...)`，空句柄使用
`A20_HANDLE_NULL`，不能用 0 代替。远端不能接收本机句柄、文件路径或进程指针。

## 3. 自定义算子

在静态 `a20_work_operator_t` 表中注册唯一 ID、真实 flags、输入/输出限额和
回调 `uint32_t run(void *, const a20_work_request_t *, uint8_t *, uint32_t *)`。
输出长度参数初值是容量，成功时改为精确字节数。所有字段显式小端编码，不
发送 C 结构体映像。回调应按输入长度验证格式，并合作遵守 budget/canceled；
分配和递归都应有明确上限。

只有无不安全副作用且允许不确定失败后再次执行，才能声明 REPLAY_SAFE。
ASSOCIATIVE 表示可结合归约，不表示交换律。输出过大应返回 NO_SPACE，不能
静默截断。默认 QUERY 现在包含 5 个算子、104 B 正文，接收缓冲须容纳 CW 头。

进程任务单独声明 PROCESS 能力，执行本地预配置 manifest ID；MCU 不发布该
能力。没有接收远端任意 ELF、命令路径或句柄的接口。

## 4. 图与结果

共享 `a20_job_init/begin/complete/tick` 不依赖 OS 或堆。调用者持有图、输入和
输出缓冲，直到作业终止；初始化完整验证拓扑后才修改运行状态。Native 驱动
`a20_cluster_job_run` 按能力选择、并发限额派发并关闭每个 attempt endpoint。

不提供输出缓冲时保留 8 B u64 接口；提供缓冲时支持 0–168 B 精确结果。
自定义 `combine` 只封装已提交子结果，实际归约仍发给 agent。
`a20_job_concat_parts` 用长度前缀封装，WORD_MERGE/CONCAT 共用它。
子结果必须提交一次才可进入父节点，旧 attempt 不改写结果缓冲。

现成作业入口是本地 CJ/1 `cluster.jobs`：DOT 操作 1、WORDCOUNT 操作 2。
文件客户端与 16 片/8 节点完整演示见 18 文档；图状态机与失败契约见 11-jobs。
持久队列、外部数据流、jobd 根故障恢复均是后续扩展，不由本教程暗示已经存在。


## 5. 单向业务接收

主机以独立 channel 导出 `events`，标志使用 `A20_EXPORT_ONEWAY`。服务端只接收，
不发送业务回复；LOCAL 连接仍须处理携带独立连接句柄的握手。身份配置须启用
`A20_CLUSTER_CAP_ONEWAY`，持久配置可写 `feature oneway`。MCU 则在首次握手前
用 `a20_mcu_leaf_set_sink()` 注册静态回调，不需要创建 Native channel。

下面接收已发现且绑定 epoch 的目标，避免目录查询后对端重启而误连新实例：

```c
#include "liba20rt/a20_cluster.h"
static a20_status_t connect_events(const a20_node_id_t *node, uint32_t slot,
                                   uint64_t epoch, a20_handle_t *ep) {
    return a20_cluster_connect_bound_flags(node, slot, epoch,
        A20_CONNECT_RELIABLE | A20_CONNECT_ONEWAY, ep);
}
```

调用者通过普通 `a20_channel_send()` 写入；该句柄没有 READ 权限，不能用
`channel_call`。只写句柄不会收到内部空回执。传输失败会关闭代理；本地发送
成功不能代替业务确认，也不能发送后立即关闭并认为已经送达。需要确认时，
应用可另设 CALL 查询已接纳的业务编号，再关闭写入端点。共享传输最多保持
64 个在途序号跨度，LEAF 对端总共一个在途；满时由现有 channel 阻塞或
NONBLOCK 提供背压。完整线协议、升级和测试边界见 22 文档。
