# Implementation of the Data Plane for the Kernel Cluster

For an overview of the architecture, see [Cluster Architecture Overview](overview.md), and for the SSI runtime specifications, see [runtime-ssi.md](runtime-ssi.md). This document describes the current kernel implementation. The control plane and the job plane are not considered complete just because kernel call tests are performed here.

## 1. File Responsibilities

| File | Responsibilities |
|---|---|
| `frame.c` / `frame.h` | Field-by-field encoding and decoding, v0 strict synchronization entry, v1 runtime entry, TTL/CRC forwarding |
| `transport.c` / `transport.h` | Adapter registration, RX queue, independent polling, link events, and counting |
| `route.c` | Immutable local identity, bounded routing table, hash collision rejection, configuration rollback |
| `export.c` | Service slot allocation, export references, FIFO request pairing, revocation, and replacement |
| `remote_ep.c` | General channel proxy, sending thread, in-transit requests, limited retries, and cancellation |
| `reliable.c` | Caching reservation before acceptance, session transaction deduplication, reply replay, and completion receipts |
| `transaction.h` | Session encoding and sequence number window functions shared by MCU and host |
| `operator.h` | Signed dot operators shared by MCU and host |
| `udp.c` | IPv4 socket adapter independent of the configuration process's FD lifecycle |
| `uart.c` / `uart.h` | UART master node state machine, serial TX, bounded RX, shared byte encoding and decoding |
| `kernel/mcu/leaf.c` | Static memory leaves, using the same session, deduplication, and operator logic |
| `selftest.c` / `udp_selftest.c` | Production path acceptance fixtures with boot parameter gating |

## 2. Threads and Progress

The first cluster operation performs an inertial initialization. The status transitions through uninitialized, initializing, ready, and failed. Concurrent initializers wait outside of the lock; the status is only declared ready when all three core threads have successfully completed their initialization. If initialization fails, an error is explicitly returned, and no data can be released if the data structure is missing an RX component or if there is no polling executor.

- `clx-poll` promotes a polling adapter with a predefined budget. It waits for the UDP socket to become available when idle and switches to UART and link control after a maximum of 1 ms. If data is available, it can be processed immediately; the driver should not be delayed by the service queue.
- `clx-rx` consumes a bounded frame queue, verifies the format, identity, and TTL before distributing or forwarding the frames. It wakes up the waiting thread as soon as a frame is added to the queue. The idle timeout is set to 10 ms, allowing a single RX owner to clean up and reorganize the queue. Condition checks and registrations are performed inside the RX lock, while prepare/commit/finish operations are done outside the lock to prevent confusion between queue additions and sleep states, which could cause wake-ups to be missed.
- `clx-hk` scans the queue every 250 ms and has a limited number of retries.
- Each remote proxy has one sending thread; each receives a corresponding thread.

The sending/receiving service queues use non-blocking mechanisms for message insertion. If the queue is full, the capacity is insufficient, the message exceeds the MTU, or the allocation fails, the operation must be terminated or an error must be returned. It is not allowed to discard already consumed messages and leave the caller in a permanent state of waiting. These threads still incur overhead for each connection. A shared worker pool for large-scale servers is a planned enhancement for future versions.

## 3. References and Reuse

The sending threads of the remote proxies maintain references to the proxy endpoints. Closing the shilling proxy invalidates it, releases the retry copies, and then closes the channels on both sides, waking up the caller and the sending threads. A slot can only be reused after the old thread exits and the endpoint is cleaned up. Asynchronous cancellations also check the generation number to prevent the use of a new generation of proxies.

The export registration process only adds references to one’s own peers; it never consumes references to other users’ handles. After successfully exporting a server endpoint, users should close any paired endpoints that are no longer in use. The core references should remain exported and active. The kernel fixture uses the explicit `a20_clx_export_register_owned` interface to transfer the uninstalled references of newly created pairs; this interface cannot be used for pairs that already have user handles.

The response thread is bound to a peer reference when it is created. The REPLACE operation assigns a new slot, and the old thread does not take over the request pairing for the new service. The old peer is deregistered/replaced, and an error is sent to the outstanding calls. When a service exits, its registration is revoked, and the slot count and reference are freed.

## 4. Locks and Resources

`g_clx_lock` protects metadata for identities, routing, exports, proxies, and response caches. There are separate locks for the RX ring, link statistics, UDP, and UART queues. Adapter sends, socket I/O, UART byte I/O, channel injection, and thread waiting are all performed outside of the registry locks.

The UART poller has a state machine and a byte stream; the sender only has a bounded queue. State/session/count information is published through snapshots within the lock, and registration/deregistration cannot concurrently modify the driver object with activities on the same link. Routing configuration is serialized; if the installation fails, UDP peers without a routing reference are reclaimed.

| Budget | MCU Resource File | DEFAULT | SERVER |
|---|---:|---:|---:|
| Routing | 4 | 256 | 4096 |
| Service Slots | 4 | 64 | 256 |
| Proxies | 2 | 128 | 1024 |
| Per Proxy in Progress | 1 | 64 | 512 |
| Response/Sending Retry Byte Budget | Static Leaf Buffer | 256 KiB | 4 MiB |

The host response cache can hold up to 64 items; each export has a pending FIFO of 8 items; the RX ring has 64 slots (63 frames can be queued under a circular slot arrangement). The DEFAULT value allows a single cache frame of up to 4096 B, while SERVER allows up to 65536 B, still subject to the next hop's MTU limit. Frame fragmentation/reassembly has not been implemented yet.

## 5. Current Failure Semantics and Limits

Reliable calls retain requests and retry up to 8 times. An ACK only indicates acceptance; further retries will continue until a business response is received or the maximum number of retries is exhausted. On the service side, response storage is reserved first, then the sequence window is submitted and executed; duplicate requests are delayed in storage and can be reused in the cache after a completion receipt bound to the session is received. Old, discarded sequence numbers will not be executed again.

The existing channel API does not provide a precise remote error slot for each call. Remote errors, link interruptions, deregistration, resource exhaustion, and expiration are all signaled with peer-closed/CANCELED. Process services must currently respond in a FIFO order; any out-of-order business completion requires subsequent explicit transactional interfaces.

Reliable direct connections are now available. Session discovery across gateway destination nodes, server fragmentation, control plane snapshots, and process-level Native agents have implemented pre-configured task lists, timeout termination, and restart recovery, as detailed in document 09; the actual contracts for clusterd and jobd are described in documents 10 and 11.

## 6. Replicable Verification

```bash
make check-cluster-unit
make check-cluster-loopback
python3 tools/cluster/check-cluster-udp.py --faults --restart
```

These tests verify various scenarios, including the production codec/MCU pairing, core-regression tests, and independent core failure cases. The board-level construction and the broader access control results are documented separately in document 08.
