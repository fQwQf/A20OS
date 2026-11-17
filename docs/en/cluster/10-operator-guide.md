# Cluster Deployment and Troubleshooting

This document provides the practical steps for implementation. For the cluster system's call conventions, refer to [01-abi.md](01-abi.md), and for the user-space service architecture, refer to [05-userspace.md](05-userspace.md).

The optional remote page service can be configured independently and is disabled by default; read-only/writable modes, capacity and traffic limits, initial files, and client APIs are described in [runtime-ssi.md](runtime-ssi.md). It offers explicit page snapshots and atomic commits, but it does not automatically create cross-machine shared address spaces for regular processes.

For the automatic cross-machine execution of regular Linux pthreads and transparent handling of out-of-memory situations, see [runtime-ssi.md](runtime-ssi.md). This feature has its own independent default disabling switch and resource limits. Currently, it is in a serial handover experimental mode for x86_64/UP systems and does not support parallel acceleration across multiple machines within the same process; workers require `a20.tcpmode=lwip`.

## 1. Deployment Prerequisites

Each machine has a unique, persistent 128-bit node_id, and each startup results in a non-repeating boot_epoch. Complete hosts recover their identity and seed from explicit, persistent configurations, while STM32 devices generate their identity/epoch based on the UID and a two-page Flash journal. IP addresses, UART short addresses, and service slots do not replace the persistent identity.

| Role | Configuration Role | Abilities and Responsibilities |
|---|---|---|
| Native Execution Node | Worker | RELIABLE; runs agents, clusterd, and optionally jobd |
| UDP/UART Gateway | Relay | RELAY+RELIABLE; forwards original transactions and can execute tasks simultaneously |
| Static MCU | Board-Level Leaf | LEAF+RELIABLE; pre-configured operators, single cache, dedicated UART periodic pump |

The MCU does not load ELF files or run file configuration supervisors; it uses the same CL/CW protocols and capability queries as the complete host. Changes to different resource files do not affect the results of operators or the semantics of transactions.

## 2. Building and Starting

```sh
make ARCH=riscv64 native-cluster-agent
make ARCH=x86_64 CLUSTER_UART_COM2=1 dev-build
PATH=/path/to/arm-toolchain/bin:$PATH make ARCH=armv7m BOARD=stm32f103 \
  STM32_FLASH_KB=64 STM32_RAM_KB=20 STM32_CLUSTER_PORT=2 kernel-only
```

After replacing the IDs, seed addresses, and architecture binaries in the configuration with the actual machine values, save the configuration persistently. Examples can be found in `docs/assets/cluster/configs/worker-rv.conf` and `relay-x86_64.conf`. In the target Native environment:

```sh
/bin/cluster-supervisor-rv /bin/cluster.conf
/bin/cluster-wordcount-rv /tmp/input.txt
```

The supervisor pre-verifies the configurations and all service images before setting up immutable identities, installing seeds, and starting the services. There can only be one `cluster.supervisor` in the same system. Each sub-service restarts independently in a graceful manner; the supervisor itself is started and recovered by the init/outer manager or operator. The repository does not automatically replace the init strategies for all architectures. The executable paths for process tasks come from the local inventory and cannot be passed in via the network or local handles.

The STM32 console uses USART1, while the cluster uses USART2 (PA2/PA3) or USART3 (PB10/PB11); the head node's COM2 uses a baud rate of 115200 with an 8N1 serial converter. Both parties use raw binary data and terminal line break conversion cannot be enabled. For specific firmware parameters, Flash erasure, and identity recovery procedures, refer to documents 12 and 13. Upgrading the firmware does not allow the use of the original node_id after erasing the persistent epoch data; maintenance should follow the reconfiguration requirements outlined in document 12.

For one-way business communication, the `feature oneway` must be explicitly enabled after upgrading the relevant endpoints and relays. Services export this feature using `A20_EXPORT_ONEWAY`; the MCU registers a static sink before the initial handshake. The default configuration does not activate additional capability bits. A successful transmission only guarantees queuing locally; the application must wait for its own business completion conditions to be met before closing the connection. For the complete protocol, see [02-wire-protocol.md](02-wire-protocol.md).

## 3. Topology and Discovery

Only actual neighbors are configured with a metric of 0 as seeds. A fully interconnected topology is suitable for a small number of hosts; a tree topology is suitable for serial port leaves and gateways. Routing to indirect destinations is propagated by confirmed neighbors; not all nodes are disguised as the same direct link address. Gateways must have a RELAY, and both parties must have a return path to the original source/destination.

clusterd first confirms neighbors, then propagates bounded member information and lease routes; directories and capabilities are still verified through queries of actual endpoints. Explicit routing takes precedence over dynamic routing, and lease cleanup only removes tuples that are owned by the system. The any service only selects instances that are ALIVE, have valid leases, and whose input/operator capabilities match; polling does not guarantee that individual tasks will cover all nodes evenly. New entries are rejected when the source table, service table, and route table are full; duplicate records that are still being protected are not removed. MCUs can have up to 8 indirect sources and one in the round-trip response cache.

Complete UDP paths can be fragmented according to the resources of both parties; paths through UART are returned in 256 B frames, with a maximum reliable request/reply size of 206/202 B. Fragmentation also requires subtracting the CW header, and the operator capabilities must be checked. Do not send oversized messages to the MCU based on the server's local RAM. For related negotiations, see documents 14–16 and 19.

## 4. Troubleshooting Sequence

| Phenomenon/Step | Check Entry | Decision and Action |
|---|---|---|
| Local service not present | `cluster_snapshot(EXPORT)`, Supervisor local status | Check the path, enabled items, exit reason, and backoff time |
| Remote connection failure | `cluster_link_status` | If status is DOWN, check the seed address, serial port pins, and IDs of both parties in the handshake; if status is SUSPECT, check the heartbeat progress |
| Link is normal but still unreachable | `cluster_snapshot(ROUTE)` | Check the target’s full ID, next hop, RELAY, return route, and dynamic leases |
| Directory resolution timeout | CD/1, CM MEMBER, CS sessions | Check the final epoch, CS cookie challenges, and service directory refresh; old boot bindings cannot be reused |
| Service name found but call failed | CW QUERY and Native resource snapshots | Compare input/output, operator IDs, budget, max_inflight, and path MTU |
| Large messages not progressing | `cluster_snapshot(kind=4)` | Check shard eligibility, recombination rejections/timouts, slot availability, and byte peaks |
| Occasional cache busyness | kind=3, retry/receipt statistics | Check for unaccepted retries and abandoned receipts; the MCU’s single cache should not be forced to be evicted and then executed again |
| Link exists but frame loss is increasing | `cluster_snapshot(kind=5)` | Separate checks for malformed frames, policy-based drops, and retransmissions; verify CRC, identity/address bindings, and TTL values |
| Resources continue to grow even after stopping | kind=3 and `/proc/a20/task_lifetime` | Check whether the proxy/retry bytes are reset to zero, as well as task and zombie process recovery; a response retaining memory does not equate to a memory leak. |

The native SDK is packaged as `a20_cluster_resources`, `a20_cluster_reassembly_resources`, and `a20_cluster_link_resources`. The measurement criteria and failure reproduction methods are described in [01-abi.md](01-abi.md) and the acceptance tests.

The performance testing interface will use the image that is being measured; do not repack the same build variant during the testing period. The monitoring service is set to LOCAL_ONLY; it provides statistics but cannot be used to execute management commands remotely. There is currently no fully interactive version of clusterctl, so the above checks should be performed using the SDK or the provided acceptance procedures.

## 5. Recovery and Upgrade

After the agent is restarted, the service slot is updated, but the epoch within the same kernel remains unchanged; the client then re-parses the service information. When clusterd is restarted, it recovers from the kernel snapshot, and existing business connections continue to function. If a node is restarted, its epoch changes, and previous bindings are canceled, requiring reconfirmation and reconnection. Jobd terminates jobs based on failures; there is no persistent job log. Repeated CJ (Command Queue) submissions will create new jobs. For failures that are uncertain, only operators with the REPLAY_SAFE flag can be rescheduled; process tasks cannot be automatically rescheduled. Disabling a call simply aborts the result without guaranteeing the termination of the remote callback.

The upgrade retains compatibility between the wire v0 gold sample and v1 negotiation; mixed paths with both old and new CS systems will result in a single-frame degradation. First, verify the independent leaf/gateway small topology, and then upgrade the entire host in batches. You cannot use the upgrade process to reset identities or clear persistent epochs in order to bypass the old session check. In the absence of hardware testing, a successful firmware connection does not replace the acceptance of Flash, electrical, and continuous operational requirements.

## 6. Verification and Deployment Boundaries

```sh
make check-cluster-unit check-cluster-fuzz
make smoke-native-cluster smoke-native-cluster-supervisor
make check-cluster-reliable check-cluster-mesh
make check-cluster-hardening
python3 tools/cluster/check-cluster-gateway.py --gossip --abandon
make demo-wordcount
```

The construction, packaging, and QEMU testing of the same image variant should be performed in sequence; the underlying image cannot be altered while the process is ongoing. Each checker maintains its own output directory, which contains configuration files, logs, and hash values of the source files. For evidence indexing, refer to document 08.

CRC checks detect damage, adjacent address binding, and CS challenges prevent replay of old sessions; however, they do not provide cryptographic authentication, confidentiality, or integrity protection against hostile networks. The current trusted boundaries are controlled LANs or dedicated serial ports. Open networks require authenticated outer channels or subsequent authentication protocols; existing deployments cannot be considered secure clusters with built-in encryption. Long-term testing, production throughput, and physical board acceptance are still documented separately in document 06.
