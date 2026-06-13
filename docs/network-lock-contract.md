# Network Lock Contract

This document defines the locking rules for A20OS kernel network paths. It
applies to the socket layer in `kernel/net/`, the lwIP integration in
`kernel/net/lwip_stack.c`, and any deferred bottom-half or workqueue that
touches network state.

## Scope and Goals

A20OS runs lwIP in `NO_SYS=1` mode. A single global spinlock, `g_lwip_lock`,
serializes all lwIP core state. The socket layer adds a second spinlock,
`g_net_lock`, to protect the per-socket message queues, waiters, and registry.

The goals of this contract are:

- Prevent deadlocks between socket system calls, lwIP callbacks, and driver paths.
- Keep interrupt and scheduler latency low by forbidding blocking operations
  under `g_lwip_lock`.
- Make it safe to run socket send/recv/connect/listen/accept tests concurrently.
- Document how deferred bottom-halves interact with both locks.

## Locks

### `g_lwip_lock`

- Defined in `kernel/net/lwip_stack.c` as a `spinlock_t`.
- Protects all lwIP core state: PCB lists, pbufs, timeout lists, netif state,
  ARP/DNS/DHCP state, and lwIP statistics.
- Acquired through `a20_lwip_lock()` and released through `a20_lwip_unlock()`.
- `a20_lwip_lock()` disables local interrupts and acquires the spinlock;
  `a20_lwip_unlock()` restores the previous interrupt state.
- Every raw lwIP API call must run while this lock is held.

### `g_net_lock`

- Declared in `kernel/net/socket_internal.h`.
- Protects `g_sockets[]`, per-socket fields, message queues, accept queues,
  ephemeral port allocation, and socket waiters.
- Must be acquired with `spin_lock_irqsave(&g_net_lock)`.

### Global Order

The lock order for network paths is:

```text
g_lwip_lock -> g_net_lock
```

`g_lwip_lock` is always the outer lock. A lwIP callback runs under the implicit
`g_lwip_lock` context and may then acquire `g_net_lock`. No path may hold
`g_net_lock` and then acquire `g_lwip_lock`.

This order is consistent with `kernel/include/core/lock.h`, which documents
`g_lwip_lock -> virtio-net nonblocking send/recv paths only`.

## Lock-Safe Socket Entry Points

The following sections describe the required locking discipline for each socket
operation family. Implementation must match these rules.

### Socket creation and destruction

`net_inet_socket_init()` and `net_inet_socket_destroy()` must hold
`g_lwip_lock` for the entire duration. They create or remove lwIP PCBs, set
callbacks, and configure TCP options. No `g_net_lock` access is needed unless
the socket is being registered, which happens after the PCB is set up.

### Bind

`net_inet_bind_pcb()` parses the user address without holding any lock, then
acquires `g_lwip_lock` only for the `udp_bind()`, `raw_bind()`, or
`tcp_bind()` call. The socket registry does not change during bind.

### Connect

Stream connect has three phases:

1. Local target resolution. If the destination is a local address, the path
   holds `g_net_lock` while searching the listener table and building the
   paired socket. No `g_lwip_lock` is held.
2. Remote TCP connect. After resolving the address, the path acquires
   `g_lwip_lock`, calls `tcp_connect()` with the connected callback, and
   releases `g_lwip_lock`.
3. Blocking wait. The caller drops all locks and blocks on `g_net_lock` using
   `net_block_on_socket_locked()`. The connected callback wakes the waiter
   through `g_net_lock`.

UDP and RAW connect follow the same pattern as bind: parse outside the lock,
then acquire `g_lwip_lock` only for `udp_connect()` or `raw_connect()`.

### Listen and accept

Listen sets up a TCP PCB for listening. The listen call must hold
`g_lwip_lock` for the `tcp_listen()` transition and for installing the accept
callback.

Accept is a `g_net_lock` only operation. It pops a pre-created child socket
from the listener accept queue. If a child is returned, the caller later calls
`net_inet_accept_child_ready()`, which acquires `g_lwip_lock` to call
`tcp_backlog_accepted()`.

### Send

The send path has different behavior for local sockets and remote sockets.

For local UDP loopback or connected local sockets, the path holds `g_net_lock`
while enqueueing data into the destination socket. If the destination queue is
full and the call is blocking, it drops `g_net_lock`, blocks, and retries.

For remote UDP, RAW, or TCP sends, the path must:

1. Ensure the socket is bound while holding `g_net_lock` if an ephemeral bind
   is needed.
2. Acquire `g_lwip_lock`.
3. Allocate a pbuf, copy data, call `udp_sendto()`/`udp_send()`/`tcp_write()`
   plus `tcp_output()`, and release `g_lwip_lock`.

`net_inet_send_tcp()` polls lwIP progress without holding any lock between
iterations, then acquires `g_lwip_lock` only for `tcp_sndbuf()`,
`tcp_write()`, and `tcp_output()`.

### Recv

Recv is a `g_net_lock` only operation. It dequeues messages from the socket
receive queue. If the queue is empty and the call is blocking, it drops the
lock, blocks on `net_block_on_socket_locked()`, and retries.

When recv drains TCP data, the caller later calls `net_tcp_recved()`, which
acquires `g_lwip_lock` to update the TCP window.

## lwIP Callback Rules

lwIP callbacks run with `g_lwip_lock` already held by lwIP. The callback must
not:

- Block or sleep.
- Call `kmalloc()` or `kfree()`.
- Call into VFS, scheduler, or any path that may acquire another spinlock
  unless that path is explicitly documented as nonblocking and lock-order safe.
- Acquire `g_lwip_lock` recursively.

The current callbacks in `kernel/net/socket_inet.c` violate the `kmalloc`
rule. They allocate receive buffers while holding `g_lwip_lock`. The cleanup
plan is described in the Deferred Bottom-Half section.

### Allowed callback work

Callbacks may perform only lightweight, bounded work:

- Copy a small amount of data out of a pbuf into a preallocated per-PCB buffer.
- Update a small number of socket state flags.
- Wake a waiter through `g_net_lock` and `proc_make_ready()`.
- Free the incoming pbuf.

All heavy work, including memory allocation, queue insertion, and large data
copies, must be deferred to a bottom-half.

## Deferred Bottom-Half Design

The P1 I/O wakeup decision is to use a deferred bottom-half / workqueue for
network completion handling. This section defines how that bottom-half must
interact with `g_lwip_lock` and `g_net_lock`.

### Bottom-half responsibilities

The network bottom-half performs the work that is currently done directly
inside lwIP callbacks:

- Allocating `net_msg_t` entries and copying payload data.
- Enqueuing received messages into the socket receive queue.
- Updating socket flags such as `closed`, `connected`, and `tcp_connecting`.
- Waking blocked waiters through `g_net_lock`.

### Top-half / bottom-half split

The lwIP callback becomes a minimal top-half:

1. Inspect the pbuf and determine the target socket.
2. If a preallocated per-PCB staging buffer is available, copy the pbuf into it.
3. Record the event type and any small metadata in a lockless per-PCB ring or
   atomic flag.
4. Schedule the bottom-half.
5. Free the pbuf and return.

The bottom-half runs later from a workqueue context:

1. Acquire `g_net_lock`.
2. Process all pending events for the socket.
3. Allocate `net_msg_t` entries and copy payload data.
4. Wake waiters.
5. Release `g_net_lock`.

### Lock interaction

The bottom-half must never hold `g_lwip_lock`. It runs with only `g_net_lock`
held. This preserves the global order because the top-half runs under
`g_lwip_lock` and does not acquire `g_net_lock`, while the bottom-half runs
under `g_net_lock` and does not acquire `g_lwip_lock`.

If a bottom-half needs to call lwIP, for example to update a TCP window or
to close a PCB, it must drop `g_net_lock`, acquire `g_lwip_lock`, perform the
lwIP call, release `g_lwip_lock`, and reacquire `g_net_lock`. It must not hold
both locks at the same time.

### Ordering between top-half and bottom-half

A per-PCB sequence counter or ring buffer guarantees that bottom-half
processing sees events in the order the top-half enqueued them. The top-half
may run in interrupt context, so the ring must be interrupt-safe on the
producer side. The consumer side runs only in workqueue context.

## kmalloc Rules Under lwIP Locks

`kernel/include/core/lock.h` forbids memory allocation while holding a device
or lwIP lock unless the callee is documented nonblocking. The current network
code allocates memory inside lwIP callbacks, which breaks this rule.

The corrected rules are:

- Do not call `kmalloc()`, `kfree()`, `net_msg_alloc()`, or any slab allocator
  function while holding `g_lwip_lock`.
- Preallocate per-PCB staging buffers at socket creation time so the top-half
  can copy pbuf data without allocating.
- Move all `net_msg_t` allocation and payload copying to the bottom-half,
  which runs without `g_lwip_lock`.
- If a code path must allocate while conceptually inside a lwIP critical
  section, drop `g_lwip_lock`, allocate, and reacquire it. This is only safe
  when the local PCB state does not need to stay stable across the drop.

## Migration Checklist

When updating the network implementation to follow this contract, verify each
item:

- [ ] lwIP callbacks no longer call `kmalloc()` or `kfree()`.
- [ ] lwIP callbacks no longer acquire `g_net_lock`.
- [ ] lwIP callbacks only perform bounded work and schedule a bottom-half.
- [ ] Bottom-half runs with `g_net_lock` only and does not hold `g_lwip_lock`.
- [ ] Socket send/recv/connect/listen/accept paths follow the lock order in
      this document.
- [ ] `a20_lwip_poll_locked()` is still safe to call with `g_lwip_lock` held.
- [ ] Driver paths under `g_lwip_lock` remain nonblocking.
- [ ] Concurrent socket stress tests pass without lock-order warnings.
