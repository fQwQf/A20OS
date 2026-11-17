# Single System Image (SSI) and Distributed Runtime Specifications

The A20OS Single System Image (SSI) mechanism allows execution environments across different physical hosts to present as a logically continuous, unified runtime system. In an SSI environment, thread scheduling, task namespaces, object handle resolution, and remote memory management all work together under the abstraction of a unified Execution Domain.

---

## 1. Unified Execution Domain Abstraction

A20OS divides computing resources into execution domains:

* **Shared Domain**: Multi-core environments on the same hardware platform with memory consistency, sharing a common address space and task tables.
* **Federated Domain**: Heterogeneous nodes connected via networks or serial buses, which enable cross-domain calls through a distributed object directory and proxy handles.

### Runtime Namespace and Handle Resolution

```mermaid
flowchart LR
    Caller["用户进程 (Caller)"] --> Handle["本地句柄 (Handle)"]
    Handle --> Proxy["代理端点 (Proxy EP)"]
    Proxy --> Directory["对象目录服务 (Object Directory)"]
    Directory --> Remote["远程目标执行域 (Remote Domain)"]
```

1. **Name Resolution**: Each released service has a globally unique 64-byte name, which is used for management of service publication and leasing through `runtime_bind` and `objectd`.
2. **Proxy Handles**: When a client connects to a remote service, it receives a local handle. The underlying operations of this handle (such as `channel_call` and `task_wait`) are transparently redirected to the remote execution proxy.

---

## 2. Configurable Remote Memory and Page Sharing

For data-intensive applications across hosts, the cluster SSI runtime provides a controlled mechanism for cross-host page sharing (with configurable remote memory services):

* **Snapshot and Transaction Mode**: To avoid uncontrollable remote lock contention, explicit page snapshots are used for data retrieval and transaction commit.
* **Deterministic Fault Recovery**: If a remote node goes offline or the network is partitioned, the local memory view is immediately rolled back to the most recent confirmed snapshot version, preventing deadlocks or dirty reads.

---

## 3. Transparent Thread Slicing

When scheduling the computational load of the same process across two or more machines, a **serial thread handover mechanism** is employed:
* The current thread slice obtains CPU execution rights on one node, while the corresponding threads on other nodes remain in a dormant state.
* At predetermined synchronization checkpoints or system call boundaries, the thread context (including register sets and changes to dirty pages) is packaged, serialized, and handed over to the next node.
* This approach provides a deterministic verification path for lightweight cross-machine collaboration without introducing the complex hardware overhead associated with shared memory in parallel systems.
