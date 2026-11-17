# Native Cluster ABI Usage Guide

Seven system calls have been integrated with the actual kernel data plane, with the range of addresses `0x0520`–`0x0526`. The authoritative definitions for these structures are found in `kernel/include/abi/native/types.h`, while the user-defined wrappers are located in `user/liba20rt/a20_cluster.h`. The protocol version and the Native parameter structure’s version are two different versions; currently, the parameter header is filled with `size=sizeof(args), version=1`.

## 1. General Rules

Parameters are passed through a single structure pointer, and the function returns a non-negative success value or a negative A20 error code. The node_id is a 16-byte persistent identifier: all zeros represent the LOCAL node, and all ones (ff) represent a reserved broadcast value; neither can be used as the identifier for a local or a regular remote node. The hash function used is FNV-1a32 and is only for indexing purposes to prevent routing installation conflicts.

Identity configuration, export, and route modification require cluster management privileges, whereas connection, event subscription, and status querying do not require such privileges. Proxy handles are only allowed to transmit data and cannot be used to transfer local handles across machines.

## 2. The Seven Calls

| syscall | Input Parameters | Successful Output |
|---|---|---|
| `cluster_set_self` | node_id, caps | 0; Only allows to be executed successfully once |
| `cluster_export` | channel, service_name, name_len, flags | New slot |
| `cluster_connect` | node_id, slot or local service name, flags, timeout_ms | Ordinary channel handle |
| `cluster_route` | ADD/DEL/REPLACE/DEL_MATCH/LEARN, node_id, transport_id, next_hop, metric | 0 |
| `cluster_event_subscribe` | Event mask | EventQ handle |
| `cluster_link_status` | node_id; LOCAL indicates summary | Status/count is written back as a parameter |
| `cluster_snapshot` (0x0526) | kind/index/generation | Current identity, resource file, running resource count, or snapshot of a single route/service |

The `set_self` caps utilize the RELAY, RELIABLE, LEAF bits, as well as the explicitly enabled ONEWAY bit; LEAF and RELAY are mutually exclusive. Information on one-way receive interfaces, capability upgrades, and admission semantics can be found in the [Wire Protocol Specification](02-wire-protocol.md) and the [Operations Guide](10-operator-guide.md). RELIABLE represents the actual capability of the data plane and is included with the kernel release. Whether a reliable connection can be established also depends on the version and capabilities of the peer. The role is not fixed by the processor model or resource profile.

## 3. Export and Reference Ownership

```c
a20_channel_pair_t pair;
a20_channel_create(&pair);
uint32_t slot;
a20_cluster_export(pair.endpoints[0], "echo", 0, &slot);
/* 成功后不再从配对端点读取，避免与内核回复线程竞争。 */
a20_hdl_close(pair.endpoints[1]);
/* 服务在 pair.endpoints[0] 接收请求并按顺序回复。 */
```

Registration increases the core's own peer references without consuming references to another user's handle. After a successful export, the application should close any pairing endpoints that are no longer in use. When a server endpoint is closed, the core revokes the export and cancels any unacknowledged transactions. If the export fails, no existing user handles are consumed.

The name length must be between 1 and 64 characters. If a registration with the same name is found without the REPLACE option, EXISTS is returned; with REPLACE, the old export is closed and a new slot is allocated. Slots are assigned monotonically and cannot be reused; if all slots are occupied, the request is denied, and an old slot will not accidentally point to a new service. The LOCAL_ONLY option prevents remote access. The WORK bit (bit 2) indicates that the service supports CW/1 for capability directory recognition; if not set, clusterd will not send CW queries to the service. Services must respond in the order they receive requests; currently, there are no interfaces for out-of-order service completion. The ONEWAY bit (bit 3) means that exports only accept SEND requests and cannot be used in combination with WORK. Connections must have both RELIABLE and ONEWAY set; the response should only include the channel handle; regular CALL connections and one-way exports cannot be mixed.

Local connections use an independent channel pair, with the connection endpoint being passed to the service as a handle. Services that support local connections must accept this handshake and serve the new endpoint; they cannot treat messages of zero length as service requests.

## 4. Routing and Transport

| transport_id | next_hop |
|---:|---|
| 0 loopback | 4 bytes of virtual node number |
| 1 UDP | IPv4 address (4 bytes) + network order port (2 bytes) |
| 2 UART | Little-endian short address (2 bytes) |

For example, `10.0.3.3:44020` is represented as `{10,0,3,3,0xab,0xf4}`. The ADD command returns EXISTS if the entry already exists; REPLACE performs an upsert; DEL returns NOT_FOUND if the entry does not exist. Configuration conflicts or invalid parameters, a full table, hash collisions, or adapter startup failures should not result in the creation of unusable new routes. Deleting or changing the next-hop will cancel the old proxy for that node.

Loopback virtual nodes share the local export table and are only for testing purposes. UDP routing uses non-blocking sockets and handshakes; UART routing requires board-level registration of byte drivers, and it cannot be assumed that a physical serial port is connected just by installing the routing.

## 5. Connections and Failures

```c
a20_handle_t remote;
a20_status_t st = a20_cluster_connect(&peer, slot, NULL,
                                      A20_CONNECT_RELIABLE, 0, &remote);
```

Remote connections via slot require that routes have been configured. For reliable connections, the peer at the other end must support both wire v1 and RELIABLE protocols; peers of version 0 can still use regular connections. Remote connections by service name using `service://any/name` require subsequent control plane resolution, as currently, unimplemented name services cannot be treated as kernel functionality.

After a successful return, the caller can use regular channel functions such as send/recv/call. The server can be shut down, the link can become DOWN, the queue can be exhausted, the MTU can be exceeded, an error response can be received, the timeout can be reached, or the number of retries can be exhausted—all of these situations will be signaled through the peer-closed event. The existing syscall for this is `-A20_ERR_CANCELED`. Detailed remote errors are retained at the protocol layer; there is currently no interface for querying errors on a per-call basis. A successful send only indicates that the request has entered the local queue, but it does not guarantee that the remote execution will be successful.

The current default timeout for remote connections is 30 seconds; reliable calls allow for up to 8 retries. The `timeout_ms` parameter has not yet been integrated into the complete link establishment/retry timeout control, so non-zero values cannot be considered as a guaranteed timeout. After each connection failure, node restart, or disconnection, the service must be re-resolved and a new proxy must be established; old handles cannot be reused.

## 6. Events and Status

The event mask includes LINK_UP, LINK_DOWN, ROUTE_LOST, and EXPORT_DROPPED. Events are delivered through `a20_event_notify` via the EventQ, with the event type being `32 + kind`, data0 containing the node hash, and data1 encoding the reason for the event. Subscribers still need to handle queue overflows and re-obtain the status.

The link status includes information such as state, rtt_us, the number of frames, the number of packets lost, the number of retransmissions, and the age of the HELLO messages. Loopback links are usually in the UP state; a successfully sent packet does not necessarily mean the link is in the UP state for real links that have not been established. Details about UDP SUSPECT/RTT and the interface for rebuilding the full status after a restart are still to be expanded; the control plane cannot rely solely on potentially lost incremental events to rebuild the complete status.

## 7. Resource Profiles and Verification

`CLUSTER_PROFILE=1/2/3` corresponds to MCU, DEFAULT, and SERVER, respectively. The MCU uses a mandatory profile 1 for construction, while the default profile 2 is used for complete hosts. For resource budgets, see [03-kernel-impl.md](03-kernel-impl.md). The NOMMU leaf node runs the same protocol and operators directly and does not require user-mode Native system calls.

```bash
make check-cluster-unit
make check-cluster-loopback
make smoke-native-cluster
python3 tools/cluster/check-cluster-udp.py --faults --restart
```

The acceptance status, physical hardware boundaries, and remaining functions can be found in [../status/current/cluster-implementation-status.md](../status/current/cluster-implementation-status.md).

The read-only enumeration of `cluster_snapshot` and the recovery protocol are described in the [User Space Service Specifications](05-userspace.md). Values of kind=3, 4, and 5 correspond to running resources, reorganizing resources, and aggregating link counts, respectively, all using the same 192 B packet format. Link statistics now include counts of malformed packets that were previously only stored in the kernel. For more information on metrics and diagnostic boundaries, see the [Operations Guide](10-operator-guide.md).

## cluster_connect v2: Binding startup sessions obtained from resolution

The parameter layout of v1 and the requirement for reserved=0 must remain unchanged. v2 uses `a20_cluster_connect_args_v2_t`: a u64 value expected_boot_epoch is appended to the full v1 base. The base.size must cover the entire v2 format, base.version must be set to 2, RELIABLE must be enabled, and epoch must be non-zero. Parameters shorter than v2, an epoch of zero, or the absence of RELIABLE will result in an INVALID_ARGUMENT response.

For LOCAL connections, the kernel uses the local boot epoch; for remote connections, the destination session negotiated before creating the proxy is checked. Inconsistency will result in a NODE_UNREACHABLE response, and old services resolved from directories cannot be dispatched to nodes that have restarted. Existing proxies will continue to use this session, and subsequent restarts will follow the existing link cancellation rules. The SDK entry point is `a20_cluster_connect_bound`; it is used by service URIs and job drivers.

### Dynamic routing operation extensions

The original structure of `cluster_route` and the syscall number remain unchanged, with the addition of op=3 `DEL_MATCH` and op=4 `LEARN`. DEL_MATCH atomically deletes a route only when the current node, transport, next_hop, length, and metric are all identical; if there is no match, a BUSY response is returned; if there is no target, a NOT_FOUND response is given. LEARN is used to add or replace routes marked with `A20_ROUTE_DYNAMIC=0x80000000`. It refuses to overwrite explicit routes (marked as EXISTS), and the passed metric must include this flag. The lower 31 bits of the metric represent the propagation distance. Administrator privileges remain the same. Explicit REPLACE can override dynamic routes; old daemons should not accidentally delete new user configurations during cleanup.
