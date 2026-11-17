# User-space Cluster Services and Interfaces

There are already native components such as `clusterd`, `cluster-agent`, and `jobd`, which use the same protocols and resource budgets as the MCU leaf nodes. The complete host runs these services as ordinary native processes, while the MCU invokes the static agents within its periodic scheduling mechanism; both components execute the CW/1 protocol. To build these components, use the command `make ARCH=riscv64 native-cluster-agent`; replace ARCH with the appropriate architecture for other host types. Information on firmware construction and various acceptance criteria can be found in [../status/current/cluster-implementation-status.md](../status/current/cluster-implementation-status.md).

| Component | Actual Responsibilities | Detailed Contract |
|---|---|---|
| clusterd | Rebuilds members from seed and kernel snapshots; direct/indirect detection; capability catalog, leases, any selection | Control plane services and dynamic routing propagation |
| cluster-agent | CW queries, registerable operators, explicit inventory processes, deadline handling | Executes agents and manages the operator catalog |
| jobd | Submits local, bounded DOT/word frequency jobs; distributes tasks to remote shards; reschedules failed jobs; performs binary tree reduction | Distributed shard reduction scheduler |
| cluster-supervisor | Maintains persistent configurations; enables independent restarts of the three services; handles backoff and exit recovery | [10-operator-guide.md](10-operator-guide.md) |
| cluster-wordcount | Submits word frequency jobs from files and prints sorted results | [cluster-apps.md](cluster-apps.md) |
| Native SDK | Parses fixed directories; supports CM/1 with any selection; binds connections for startup sessions and drives jobs | `user/liba20rt/a20_cluster*.h` |
| MCU leaf | Uses the same CW protocol; maintains a static operator table; handles capability queries; provides reliable single-frame responses | [04-transports.md](04-transports.md) |

## Message Encoding

CL payloads can carry applications' own protocols; the existing CW, CD, CM, and CJ protocols are explicit, little-endian protocols with fixed fields and versions, and do not transmit memory images of C structures. The original TLV format is still compatible data, but it cannot be described in the format currently used by clusterd/jobd. Native local handles are only used for local connections and are not serialized for remote transmission.

## Recovery and Restrictions

`clusterd` restarts from the existing kernel state, ensuring that existing business connections are not interrupted due to the replacement of the control plane. A agent restart creates new service slots, causing old connections to fail; however, the epoch number remains unchanged within the same kernel. The exit of the jobd master process terminates the jobs, with no job logs or automatic recovery mechanisms in place. Retry operators guarantee at least one execution and result deduplication; process tasks do not allow automatic rescheduling in case of uncertain failures.

Bounded member propagation and lease routing have been integrated into clusterd, while persistent configurations and native monitoring are now managed by `cluster-supervisor` (see [10-operator-guide.md](10-operator-guide.md)). Generalized byte results, custom encapsulation reductions, CONCAT, and word frequency calculations are all executed using the same scheduler (see [cluster-apps.md](cluster-apps.md)). The following design goals remain: universal TLV job submission, multi-job persistent queues, external data stream processing, and larger-scale, physically mixed deployments. The current implementation has separated goals from runtime evidence, continuing to advance according to the unified runtime architecture specifications.
