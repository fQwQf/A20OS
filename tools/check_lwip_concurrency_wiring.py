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
    filter_body = _body("kernel/net/lwip_stack.c", "a20_lwip_filter")
    _ordered(filter_body, "conntrack filter wrapper",
             "a20_lwip_shared_lock(A20_LWIP_SHARED_CT, NULL)",
             "netfilter_input(frame, len, idx)",
             "netfilter_output(frame, len, idx)",
             "a20_lwip_shared_unlock(A20_LWIP_SHARED_CT, NULL, flags)")
    if "a20_lwip_lane_lock" in filter_body or "a20_lwip_lock(" in filter_body:
        raise ContractError("conntrack filter wrapper must not acquire a core lane/global core lock")

    enqueue = _body("kernel/net/lwip_stack.c", "a20_lwip_rx_enqueue_locked")
    _ordered(enqueue, "multi-lane ingress filter/publication",
             "a20_lwip_filter(st->rx_frame, (size_t)len, st->idx, 1)",
             "NETFILTER_DROP", "a20_lwip_frame_lane(n, st->rx_frame",
             "net_lane_rx_put(lane, n, st->rx_frame")

    legacy_rx = _body("kernel/net/lwip_stack.c", "a20_lwip_process_netif_rx_tx_locked")
    _ordered(legacy_rx, "single-lane ingress filter/pbuf delivery",
             "a20_lwip_filter(st->rx_frame, (size_t)len, st->idx, 1)",
             "NETFILTER_DROP", "pbuf_alloc(PBUF_RAW", "pbuf_take(p, st->rx_frame",
             "n->input(p, n)")

    tx = _body("kernel/net/lwip_stack.c", "a20_lwip_linkoutput")
    filter_calls = [m.start() for m in re.finditer(r"a20_lwip_filter\(", tx)]
    send_calls = [m.start() for m in re.finditer(r"st->ops->send(?:_sg)?\(", tx)]
    if len(filter_calls) != 2 or len(send_calls) != 2 or any(
            f >= s for f, s in zip(filter_calls, send_calls)):
        raise ContractError("both linkoutput paths must filter their staged frame before device send")

    virtio = _body("kernel/drivers/net/virtio_net.c", "virtio_net_irq_handler")
    _ordered(virtio, "virtio IRQ ingress bridge",
             "a20_lwip_ingress_lock()",
             "a20_lwip_process_netif_irq_locked(net->slot)",
             "a20_lwip_ingress_unlock(flags)")
    header = (ROOT / "kernel/include/net/lwip_stack.h").read_text(encoding="utf-8")
    if "void a20_lwip_process_netif_irq_locked(int net_idx);" not in header:
        raise ContractError("lwip_stack.h does not export the IRQ ingress API")
    irq_body = _body("kernel/net/lwip_stack.c", "a20_lwip_process_netif_irq_locked")
    if "a20_lwip_rx_enqueue_locked(n, 0)" not in irq_body:
        raise ContractError("multi-lane IRQ API no longer stages frames on owner queues")

    drain = _body("kernel/net/lwip_stack.c", "a20_lwip_lane_drain")
    _ordered(drain, "owner-lane frame processing",
             "net_lane_rx_claim(lane)", "a20_lwip_lane_lock(lane)",
             "net_lane_ctx_push(lane)", "a20_lwip_lane_input_one(lane, n, frame, len)",
             "net_lane_ctx_pop(prev)", "a20_lwip_lane_unlock(flags)",
             "net_lane_rx_consume(lane)")

    accept = _body("kernel/net/socket_inet.c", "lwip_tcp_accept_cb")
    lock = accept.find("a20_lwip_shared_lock(A20_LWIP_SHARED_LISTENER, listener_key)")
    head_read = accept.find("__atomic_load_n(&st->head, __ATOMIC_RELAXED)")
    publish = accept.find("__atomic_store_n(&st->head, head + 1, __ATOMIC_RELAXED)")
    schedule = accept.find("net_inet_bh_schedule(s)")
    success_unlocks = list(re.finditer(
        r"a20_lwip_shared_unlock\(\s*A20_LWIP_SHARED_LISTENER\s*,\s*"
        r"listener_key\s*,\s*listener_flags\s*\)", accept))
    success_unlock = next((m.start() for m in reversed(success_unlocks)
                           if schedule >= 0 and m.start() < schedule), -1)
    if min(lock, head_read, publish, success_unlock, schedule) < 0:
        raise ContractError("accept stage is missing listener guard, head publication, unlock, or BH schedule")
    if not (accept.find("const void *listener_key = s->tcp;") < lock < head_read < publish <
            success_unlock < schedule):
        raise ContractError("accept stage must lock stable listener PCB, publish head, unlock, then schedule BH")
    if "tcp_abort(" in accept:
        raise ContractError("accept callback must return ERR_MEM and leave child aborting to TCP core")

    listen = _body("kernel/external/lwip/src/core/tcp_in.c", "tcp_listen_input")
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

    tcp_source = (ROOT / "kernel/external/lwip/src/core/tcp_in.c").read_text(encoding="utf-8")
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
