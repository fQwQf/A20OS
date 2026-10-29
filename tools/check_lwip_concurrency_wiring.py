#!/usr/bin/env python3
"""Check the actual ordering contracts around sharded lwIP ingress/egress."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class ContractError(ValueError):
    pass


def _body(path: str, name: str) -> str:
    source = (ROOT / path).read_text(encoding="utf-8")
    signature = re.compile(
        rf"(?m)^[ \t]*(?:static\s+)?[\w\s\*]+\b{re.escape(name)}"
        r"\s*\([^;{}]*\)\s*\{"
    )
    match = signature.search(source)
    if not match:
        raise ContractError(f"function {name} not found in {path}")
    start = source.find("{", match.start(), match.end())
    depth = 0
    i = start
    state = "code"
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if ch == "/" and nxt == "*":
                state = "block-comment"
                i += 2
                continue
            if ch == "/" and nxt == "/":
                state = "line-comment"
                i += 2
                continue
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[start + 1:i]
        elif state == "block-comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
        elif state == "line-comment":
            if ch == "\n":
                state = "code"
        elif state in ("string", "char"):
            if ch == "\\":
                i += 2
                continue
            if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        i += 1
    raise ContractError(f"unclosed function body for {name} in {path}")


def _rust_body(path: str, name: str) -> str:
    """Extract an `fn name(...) { ... }` body from a Rust source file."""
    source = (ROOT / path).read_text(encoding="utf-8")
    match = re.search(rf"(?m)^\s*(?:pub(?:\([^)]*\))?\s+)?(?:unsafe\s+)?"
                      rf"(?:extern\s+\"C\"\s+)?fn\s+{re.escape(name)}\s*\(", source)
    if not match:
        raise ContractError(f"function {name} not found in {path}")
    start = source.find("{", match.end())
    if start < 0:
        raise ContractError(f"function {name} has no body in {path}")
    depth = 0
    i = start
    state = "code"
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line-comment"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block-comment"
                i += 2
                continue
            if ch == '"':
                state = "string"
            elif ch == "'":
                # Lifetimes (`'static`) and char literals both start with a
                # quote; only treat it as a char if it closes on the next byte.
                if i + 2 < len(source) and source[i + 2] == "'":
                    state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[start + 1:i]
        elif state == "block-comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
        elif state == "line-comment":
            if ch == "\n":
                state = "code"
        elif state in ("string", "char"):
            if ch == "\\":
                i += 2
                continue
            if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        i += 1
    raise ContractError(f"unclosed function body for {name} in {path}")


def _ordered(body: str, description: str, *needles: str) -> None:
    positions = []
    for needle in needles:
        pos = body.find(needle)
        if pos < 0:
            raise ContractError(f"{description}: missing `{needle}`")
        positions.append(pos)
    if positions != sorted(positions):
        raise ContractError(f"{description}: operations are out of required order")


def check() -> None:
    filter_body = _body("kernel/net/backends/lwip/lwip_stack.c", "a20_lwip_filter")
    _ordered(filter_body, "conntrack filter wrapper",
             "netfilter_input(frame, len, idx)",
             "netfilter_output(frame, len, idx)")
    nat = (ROOT / "kernel/net/filter/netfilter_nat.rs").read_text()
    if "let _guard = CtGuard::acquire();" not in nat or "&'static mut" in nat:
        raise ContractError("conntrack must own exclusion and cannot manufacture static mutable aliases")
    if "a20_lwip_shared_lock" in filter_body:
        raise ContractError("conntrack synchronization must not depend on a backend lock")
    if "a20_lwip_lane_lock" in filter_body or "a20_lwip_lock(" in filter_body:
        raise ContractError("conntrack filter wrapper must not acquire a core lane/global core lock")

    enqueue = _body("kernel/net/backends/lwip/lwip_stack.c", "a20_lwip_rx_enqueue_locked")
    _ordered(enqueue, "multi-lane ingress filter/publication",
             "net_lane_rx_all_have_room()", "netif_device_recv(",
             "a20_lwip_filter(st->rx_frame, (size_t)len, st->idx, 1)",
             "NETFILTER_DROP", "a20_lwip_frame_lane(n, st->rx_frame",
             "net_lane_rx_put(lane, n, st->rx_frame")

    legacy_rx = _body("kernel/net/backends/lwip/lwip_stack.c", "a20_lwip_process_netif_rx_tx_locked")
    _ordered(legacy_rx, "single-lane ingress filter/pbuf delivery",
             "a20_lwip_filter(st->rx_frame, (size_t)len, st->idx, 1)",
             "NETFILTER_DROP", "pbuf_alloc(PBUF_RAW", "pbuf_take(p, st->rx_frame",
             "n->input(p, n)")

    tx = _body("kernel/net/backends/lwip/lwip_stack.c", "a20_lwip_linkoutput")
    filter_calls = [m.start() for m in re.finditer(r"a20_lwip_filter\(", tx)]
    send_calls = [m.start() for m in re.finditer(r"netif_device_send(?:_sg)?\(", tx)]
    if len(filter_calls) != 2 or len(send_calls) != 2 or any(
            f >= s for f, s in zip(filter_calls, send_calls)):
        raise ContractError("both linkoutput paths must filter their staged frame before device send")

    connect = _body("kernel/net/backends/lwip/socket_inet.c", "net_inet_connect_stream")
    connect = connect[connect.index("err_t e = tcp_connect"): ]
    _ordered(connect, "active open publishes rebucketed PCB ownership",
             "tcp_connect(lw(s)->tcp", "net_socket_lane_store(s, NET_PCB_LANE_OWNER_OF_PCB(lw(s)->tcp))",
             "a20_lwip_unlock(lwip_flags)")

    virtio = _body("kernel/drivers/net/virtio_net.c", "virtio_net_irq_handler")
    _ordered(virtio, "virtio IRQ ingress bridge",
             "a20_lwip_ingress_lock()",
             "a20_lwip_process_netif_irq_locked(net->slot)",
             "a20_lwip_ingress_unlock(flags)")
    header = (ROOT / "kernel/include/net/lwip_stack.h").read_text(encoding="utf-8")
    if "void a20_lwip_process_netif_irq_locked(int net_idx);" not in header:
        raise ContractError("lwip_stack.h does not export the IRQ ingress API")
    irq_body = _body("kernel/net/backends/lwip/lwip_stack.c", "a20_lwip_process_netif_irq_locked")
    if "a20_lwip_rx_enqueue_locked(n, CONFIG_NET_RX_IRQ_BUDGET)" not in irq_body:
        raise ContractError("multi-lane IRQ API no longer stages frames on owner queues")

    drain = _body("kernel/net/backends/lwip/lwip_stack.c", "a20_lwip_lane_drain")
    _ordered(drain, "owner-lane frame processing",
             "net_lane_rx_claim(lane)", "a20_lwip_lane_lock(lane)",
             "net_lane_ctx_push(lane)", "a20_lwip_lane_input_one(lane, n, frame, len)",
             "net_lane_ctx_pop(prev)", "a20_lwip_lane_unlock(flags)",
             "net_lane_rx_consume(lane)")

    # The callback is now a C wrapper around the Rust publication transaction.
    # Follow the delegation: assert the wrapper's contract, then the real
    # ordering inside a20_net_accept_publish().
    accept = _body("kernel/net/backends/lwip/socket_inet.c", "lwip_tcp_accept_cb")
    if "a20_net_accept_publish(s, newpcb, NET_ACCEPT_STAGE_SIZE)" not in accept:
        raise ContractError("accept callback no longer delegates publication to the stage transaction")
    if "tcp_abort(" in accept:
        raise ContractError("accept callback must return ERR_MEM and leave child aborting to TCP core")

    publish_body = _rust_body("kernel/net/backends/lwip/socket_inet_accept.rs", "a20_net_accept_publish")
    lock = publish_body.find("a20_net_accept_stage_lock(listener)")
    head_read = publish_body.find("(*(v.head as *const AtomicU32)).load(Ordering::Relaxed)")
    arm = publish_body.find("a20_net_accept_slot_arm(slot")
    fence = publish_body.find("fence(Ordering::Release)")
    publish = publish_body.find("store(head.wrapping_add(1), Ordering::Relaxed)")
    success_unlock = publish_body.rfind("a20_net_accept_stage_unlock(listener, sf)")
    schedule = publish_body.find("a20_net_accept_bh_schedule(listener)")
    if min(lock, head_read, arm, fence, publish, success_unlock, schedule) < 0:
        raise ContractError("accept stage is missing listener guard, head publication, unlock, or BH schedule")
    if not (lock < head_read < arm < fence < publish < success_unlock < schedule):
        raise ContractError("accept stage must lock stable PCB, arm the slot, publish head, unlock, then schedule BH")

    stage_lock = _body("kernel/net/backends/lwip/socket_inet.c", "a20_net_accept_stage_lock")
    if "a20_lwip_shared_lock(A20_LWIP_SHARED_LISTENER, lw(s)->tcp)" not in stage_lock:
        raise ContractError("accept stage lock no longer binds the stable listener PCB")
    stage_unlock = _body("kernel/net/backends/lwip/socket_inet.c", "a20_net_accept_stage_unlock")
    if "a20_lwip_shared_unlock(A20_LWIP_SHARED_LISTENER, lw(s)->tcp, flags)" not in stage_unlock:
        raise ContractError("accept stage unlock no longer releases the listener guard")
    bh_schedule = _body("kernel/net/backends/lwip/socket_inet.c", "a20_net_accept_bh_schedule")
    if "net_inet_bh_schedule" not in bh_schedule:
        raise ContractError("accept schedule no longer enters the bottom half")

    listen = _body("kernel/net/backends/native/protocol/src/core/tcp_in.c", "tcp_listen_input")
    alloc = listen.find("npcb = tcp_alloc(pcb->prio)")
    if alloc < 0:
        raise ContractError("tcp_listen_input no longer allocates child PCB")
    before_alloc = listen[:alloc]
    listener_locks = list(re.finditer(
        r"a20_lwip_shared_lock\(A20_LWIP_SHARED_LISTENER,\s*pcb\)", before_alloc))
    listener_unlocks = list(re.finditer(
        r"a20_lwip_shared_unlock\(A20_LWIP_SHARED_LISTENER,\s*pcb,\s*listener_flags\)",
        before_alloc))
    if len(listener_locks) != 1 or not listener_unlocks:
        raise ContractError("tcp_listen_input must use only a short backlog guard before child allocation")
    if any(match.start() < listener_locks[0].start() for match in listener_unlocks):
        raise ContractError("tcp_listen_input listener backlog guard must be released before tcp_alloc")

    tcp_source = (ROOT / "kernel/net/backends/native/protocol/src/core/tcp_in.c").read_text(encoding="utf-8")
    tcp_start = tcp_source.find("tcp_input(struct pbuf *p, struct netif *inp)")
    listen_call = tcp_source.find("tcp_listen_input(lpcb);")
    if tcp_start < 0 or listen_call < tcp_start:
        raise ContractError("tcp_input listener dispatch call not found")
    if "A20_LWIP_SHARED_LISTENER" in tcp_source[tcp_start:listen_call]:
        raise ContractError("tcp_input must not hold a listener shared guard across tcp_listen_input")


def main() -> int:
    try:
        check()
    except (OSError, ContractError) as exc:
        print(f"lwip-concurrency-wiring: FAIL: {exc}", file=sys.stderr)
        return 1
    print("lwip-concurrency-wiring: PASS (filter ordering, IRQ staging, owner-lane execution, accept-stage guard)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
