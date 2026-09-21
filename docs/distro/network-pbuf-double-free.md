# Network: first real traffic panics the kernel (pbuf double free)

## State before the test: the stack is configured

From a clean boot (`/tmp/opencode/net1.log`):

    [LWIP] initialized: IPv4 IPv6 TCP UDP RAW ICMP DHCP DNS loopif
    [INIT] Network initialized
    [DRVMOD] loaded 'virtio-net.a20drv' id=8 base=0xffff80013f927000 size=307200
    [DRIVER] registered driver 'virtio-net' (class=3)
    [VIRTIO-NET0] ready legacy=0 mac=52:54:00:12:34:56
    [DRIVER] device 'pci-1af4:1000-4' bound to driver 'virtio-net'
    [LWIP] netif en2 attached to pci-1af4:1000-4 ip=10.0.2.15 gw=10.0.2.2 dns_count=1

Guest side: `/etc/resolv.conf` is `nameserver 10.0.2.3`; `udhcpc`, `wget` and `nc` are present.

So the interface, address, gateway and DNS are all in place.  Networking is not "missing" --
it fails as soon as a packet moves.

## The bug: first traffic panics

Harness runs, in order:

    wget -T 6 -O /dev/null http://10.0.2.2/     # raw IP, no DNS involved

Output:

    Connecting to 10.0.2.2 (10.0.2.2:80)
    ========== KERNEL PANIC ==========
    lwIP assertion failed: pbuf_free: p->ref > 0
    [PANIC] arch=x86_64 cpu=0
    [PANIC] task: pid=0 name=idle state=2
    [PANIC] caller=pbuf_free+0x194 sp=~0xffff8000013d5be8
    [PANIC] backtrace:
      [0] pbuf_free+0x194
      [1] etharp_input+0x415
      [2] ethernet_input+0x2f2
      [3] a20_lwip_process_netif_rx_tx_locked+0x154
      [4] ethernet_output+0x13f200867      <- unreliable symbol, see below
      [5] driver_irq_dispatch+0x463
      [6] arch_handle_irq+0x53
      [7] ethernet_output+0x52a
      [8] kernel_main+0x503
      [9] 0x200142

`pbuf_free: p->ref > 0` means a pbuf whose refcount had already reached 0 was freed again: a
**double free of a received pbuf**.  It fires while parsing an inbound ARP packet
(`etharp_input` -> `ethernet_input`), reached from the receive path
(`a20_lwip_process_netif_rx_tx_locked`), i.e. from the virtio-net IRQ.

Two things to note:

  * the panic happens on the **first** connection attempt, so before this nothing could ever
    work -- every higher-level symptom (including Minecraft's `UnknownHostException`) sits on
    top of it;
  * the backtrace is not entirely trustworthy (`ethernet_output+0x13f200867` is nonsense), so
    the frame order below `etharp_input` should be confirmed rather than assumed.

## Next step

The likely shape is an ownership error on the receive path: whoever hands the RX pbuf to
`ethernet_input` must not free it afterwards, because lwIP's input chain already consumes and
frees it (that is exactly what `etharp_input` does with pbufs it does not keep).  Read the RX
hand-off in `a20_lwip_process_netif_rx_tx_locked` and the virtio-net driver's receive path, and
check whether the pbuf is freed by both the driver side and lwIP.

Also worth fixing while in here: the guest has no working way to inspect the interface --
`ip addr show` fails with `socket(AF_NETLINK,3,0): Protocol not supported` and
`ifconfig` fails with `ioctl 0x8912 failed: No such device`.  Neither is the cause of the
panic, but both make network debugging in the guest much harder than it needs to be.

## Candidate site, and why the obvious fix is not obviously right

The receive loop is `a20_lwip_process_netif_rx_tx_locked` (`kernel/net/lwip_stack.c:325-342`):

    for (;;) {
        int len = st->ops->recv(st->dev, st->rx_frame, sizeof(st->rx_frame));
        if (len <= 0) break;
        net_packet_rx_defer((unsigned)netif_get_index(n), st->rx_frame, (size_t)len);
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (!p) { LINK_STATS_INC(link.memerr); LINK_STATS_INC(link.drop); continue; }
        pbuf_take(p, st->rx_frame, (u16_t)len);
        if (n->input(p, n) != ERR_OK) {          /* line 338 */
            pbuf_free(p);                        /* line 339 */
            LINK_STATS_INC(link.drop);
        }
    }

`n->input` is `ethernet_input` (registered at line 234).  Line 339 looks like a double free, but
it follows lwIP's **documented** contract -- upstream's own `ethernetif_input()` is written
exactly this way: `input()` returns `ERR_OK` when it consumed the pbuf, and the caller frees it
when it did not.

That contract is only sound if `ethernet_input` never consumes the pbuf before returning an
error.  It propagates its inner error on some paths (the IPv4 path returns what `ip4_input`
returned), and `ip4_input` can free or queue the pbuf *before* returning a non-OK error.  If that
happens, line 339 frees an already-freed pbuf: the pool pbuf lands with `ref == 0`, is handed out
again, and a later `pbuf_free` -- e.g. `etharp_input`'s own -- trips `p->ref > 0`.

Two candidate ownership models, and the fix differs:

  * `ethernet_input` always consumes -> line 339 must go;
  * `ethernet_input` consumes only on `ERR_OK` -> line 339 is right, and the double free is
    inside lwIP's error path, which a `pbuf_ref` before the call would neutralise.

Do **not** guess between them.  **Resolved -- and it is neither.**  Reading the vendored lwIP
(`kernel/external/lwip/src/netif/ethernet.c`) settles it: every one of the 12 exit paths returns
`ERR_OK`, including `free_and_return`:

    /* This means the pbuf is freed or consumed,
       so the caller doesn't have to free it again */
    return ERR_OK;

    free_and_return:
      pbuf_free(p);
      return ERR_OK;

`n->input(p, n) != ERR_OK` is therefore **never true** in this loop, so line 339 is dead code,
not a double free.  (Worth stating plainly: the "obvious" fix of deleting that `pbuf_free` would
have changed nothing at all, which is why guessing here was worth refusing.)

## FIXED (12b3df6a)

lwIP is upstream, so the ownership error had to be ours.  It was: `kernel/net/socket_inet.c`
freed the pbuf **after** handing it to lwIP, and lwIP's send calls take ownership and free it
themselves --

    e = udp_sendto(s->udp, p, &ip, port);   /* udp.c frees p: 371/407/425/428/440 */
    e = raw_sendto(s->raw, p, &ip);         /* raw.c frees p: 75/113 */
    pbuf_free(p);                            /* double free */

So every UDP and raw send freed the same pbuf twice, corrupting the pbuf pool; the damage then
surfaced on an unrelated receive, as the assertion inside `etharp_input`'s own free.  Both paths
were fixed by dropping the trailing free.  The branch where lwIP is never called (no destination,
not connected) still frees the pbuf, which is correct.

Verified in a clean boot:

    PANIC count: 0
    Connecting to 10.0.2.2 (10.0.2.2:80)
    wget: can't connect to remote host (10.0.2.2): Connection refused

`Connection refused` is the right answer -- 10.0.2.2 is the QEMU gateway with nothing listening
on :80 -- and it means ARP resolution, the TCP SYN and the RST all worked.  Previously the same
command panicked at the ARP stage.  `nc -u` to the resolver also returns 0.

Worth noting: this bug was only reachable after the page-cache fix (36c14858) removed the
first-run corruption that had been masking it.

## Remaining: DNS resolution still fails

    === NETTEST hostname dns:
    wget: bad address 'example.com'
    rc=1

`/etc/resolv.conf` is readable (`nameserver 10.0.2.3`) and UDP traffic to that port reports no
error, so the query leaves but the answer is not reaching the application.  Next: check that a
UDP reply is queued to the socket that sent the query -- i.e. the socket-layer receive path for
UDP (`socket_inet.c` receive/bind handling) rather than lwIP itself, since TCP receive works.
This is what still blocks Minecraft's authentication.

Also still true: `ip addr show` fails (`AF_NETLINK` unsupported) and `ifconfig` fails
(`ioctl 0x8912`, no such device), which makes in-guest network debugging harder than it needs to
be.

`kernel/net/lwip_stack.c`, IRQ top-half (lines 346-373):

    /*
     * IRQ top-half entry for a single virtio-net instance.
     * Runs with g_lwip_lock held; performs bounded work only (descriptor ring
     * drainer, lwIP input, no kmalloc, no g_net_lock).
     */
    void a20_lwip_process_netif_irq_locked(int net_idx)
    {
        if (!g_lwip_ready) return;
        a20_lwip_signal_rx_pending();
        for (int i = 0; i < A20_NET_MAX_DEVS; i++) {          /* every device */
            a20_lwip_netif_state_t *st = &g_netif_state[i];
            if (st->dev && st->ops && st->ops->poll)
                st->ops->poll(st->dev);
        }
        for (struct netif *n = netif_list; n; n = n->next) {
            if (!n->state) continue;
            a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)n->state;
            if (st->idx == net_idx) {
                a20_lwip_process_netif_rx_tx_locked(n);       /* the same drainer */
                break;
            }
        }
    }

The comment says the entry is "for a **single** virtio-net instance", but the body polls **every**
device and then drains the target netif with the same `a20_lwip_process_netif_rx_tx_locked()` that
`a20_lwip_poll_locked()` uses over the whole netif list (line 387).  That is two independent ways
to consume the same device's receive work.

`g_lwip_lock` is a spinlock used via `spin_lock_irqsave` (lines 304/309), so on this single-CPU
image the IRQ cannot interleave *inside* the poll path -- which is exactly why the race does not
have to be simultaneous: what matters is whether the device is polled (and its frames delivered)
**twice for the same packet**, in either order, before the pbuf is handed to lwIP once too many.

Next step: read `st->ops->poll()` for virtio-net and confirm whether it also delivers received
frames into lwIP (it is called on every device by the IRQ path).  If it does, then the following
drainer re-delivers frames already consumed, and the fix is to make the IRQ path touch only its
own device -- not to change any pbuf handling.

## Why the damage must be earlier than the packet

The assertion fires inside `etharp_input`'s **own** final `pbuf_free(p)`, which means `p->ref`
was **already 0** when the packet reached `etharp_input` -- the pbuf was freed while still in
use, before that point in the same packet's processing.  Nothing in the loop between
`pbuf_alloc` (which sets `ref = 1`) and `ethernet_input` decrements a reference, so the
corruption is earlier than this packet:

  * the pbuf pool's free list is already damaged -- an earlier double free put the same pbuf on
    it twice, so `pbuf_alloc` hands out a frame another owner still holds; or
  * the RX path is entered from two contexts at once and both process the same pbuf.  Note the
    panic ran in IRQ context (`arch_handle_irq` -> `driver_irq_dispatch`) on `pid=0 idle`, and
    `a20_lwip_process_netif_rx_tx_locked` has **two** call sites (lines 369 and 387) -- if one of
    them is a poller and the other the IRQ, the "locked" contract is what needs checking first.

Next step, in this order:

  1. read the two call sites at `lwip_stack.c:369` and `:387` and confirm they cannot run
     concurrently;
  2. if they can, that is the bug and the fix is in the locking, not in the pbuf handling;
  3. if they cannot, instrument `pbuf_alloc`/`pbuf_free` with a refcount trace (and assert that
     an allocated pbuf starts at `ref == 1`) to catch whichever frame is handed out twice.
