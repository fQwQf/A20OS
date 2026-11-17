# Cluster Application Development and Distributed Workload Specifications

A20OS supports the deployment of clusters ranging from lightweight distributed streaming operators to complete Java applications. This document describes the cluster programming interface, the batch job model, and best practices for integrating complex workloads.

---

## 1. Native Cluster Service Programming Model

Distributed services use A20OS Native Channel as the communication foundation.

### Server: Export and Request Handling

```c
#include <user/liba20rt/a20_cluster.h>

int main(void) {
    // 1. 初始化集群环境并导出服务通道
    int channel_handle = cluster_export_service(MY_SERVICE_SLOT, 0);
    
    // 2. 消息处理事件循环
    while (1) {
        a20_msg_t req;
        cluster_recv(channel_handle, &req);
        
        a20_msg_t resp;
        process_request(&req, &resp);
        
        cluster_reply(channel_handle, &req, &resp);
    }
}
```

### Client: Connection and Transparent Invocation

```c
// 连接至目标节点的特定服务槽位
int channel_handle = cluster_connect(target_node_id, MY_SERVICE_SLOT);

// 发起同步请求并等待回复
a20_msg_t req = { .op = OP_COMPUTE, ... };
a20_msg_t resp;
channel_call(channel_handle, &req, &resp);
```

## 2. Batch Processing and Reduction Job Models

A20OS includes a distributed job scheduler based on tree-based reduction (see [User Space Service Specifications](05-userspace.md):
* **Slice Dispatch**: Divides large amounts of input data into independent slices, and the scheduler distributes these slices according to the capabilities and real-time load of each computing node (host or MCU).
* **Slice Retry and Fault Tolerance**: If a single slice fails to complete due to a timeout or if the target node goes offline, the scheduler automatically reschedules the slice on another available node, ensuring at most one execution of the job.
* **Result Reduction**: The results of the slice calculations are aggregated and reported step by step, supporting typical computational paradigms such as word frequency statistics (WordCount).

## 3. Practice of Integrating Complex Workloads

A20OS’s native single-core architecture has successfully verified the execution of a complete Minecraft Java client in a dual-QEMU shared environment:
* **Runtime Stack**: Uses a trimmed version of JamVM to interpret and execute Java bytecode.
* **System Call Bridging**: Maps memory allocation, time retrieval, input event listening, and simple graphic frame buffer drawing requirements of the JVM to A20OS’s native APIs.
* **Cross-Core Collaboration**: Different Java threads are seamlessly scheduled to different physical/simulated CPU nodes for collaborative execution.
