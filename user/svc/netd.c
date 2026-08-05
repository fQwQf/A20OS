/*
 * netd — userspace network daemon (hybrid-kernel netstack migration).
 *
 * Runs lwIP (NO_SYS) in user mode; the kernel keeps the virtio-net driver
 * and forwards ethernet frames over the shared rings defined in
 * netd_proto.h.  The frame VMO is passed at a fixed spawn slot.
 */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "lwip/timeouts.h"
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20c/include/stdarg.h"
#include "liba20rt/a20_syscall.h"
#include "liba20rt/a20_mem.h"
#include "../net/lwip/netd_proto.h"

#define NETD_RINGS_SLOT  (A20_NATIVE_FD_HANDLE_BASE + 71u)

static netd_rings_t *g_rings;
static struct netif g_netif;

static uint32_t ring_read(netd_frame_ring_t *r, uint8_t *out, uint32_t max)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    if (head == tail)
        return 0;
    uint32_t idx = tail & r->slot_mask;
    uint32_t off = idx * (4 + NETD_MAX_FRAME);
    uint32_t len = *(const uint32_t *)(r->data + off);
    if (len > max || len > NETD_MAX_FRAME)
        return 0;
    uint32_t i;
    for (i = 0; i < len; i++)
        out[i] = r->data[off + 4 + i];
    __atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELEASE);
    return len;
}

static void ring_write(netd_frame_ring_t *r, const uint8_t *data, uint32_t len)
{
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    if (tail - head >= NETD_RING_SLOTS)
        return; /* full: drop (NIC will retransmit) */
    uint32_t idx = tail & r->slot_mask;
    uint32_t off = idx * (4 + NETD_MAX_FRAME);
    *(uint32_t *)(r->data + off) = len;
    uint32_t i;
    for (i = 0; i < len; i++)
        r->data[off + 4 + i] = data[i];
    __atomic_store_n(&r->head, tail + 1, __ATOMIC_RELEASE);
}

static err_t netd_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    uint8_t frame[NETD_MAX_FRAME];
    uint16_t len = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        if (len + q->len > NETD_MAX_FRAME)
            return ERR_MEM;
        uint32_t i;
        for (i = 0; i < q->len; i++)
            frame[len + i] = ((const uint8_t *)q->payload)[i];
        len += q->len;
    }
    ring_write(&g_rings->tx, frame, len);
    return ERR_OK;
}

static err_t netd_netif_init(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = netd_linkoutput;
    netif->mtu = 1500;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    /* 52:54:00:12:34:56 (QEMU user-net default) */
    netif->hwaddr[0] = 0x52; netif->hwaddr[1] = 0x54;
    netif->hwaddr[2] = 0x00; netif->hwaddr[3] = 0x12;
    netif->hwaddr[4] = 0x34; netif->hwaddr[5] = 0x56;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;
    return ERR_OK;
}

static void netd_rx_drain(void)
{
    uint8_t frame[NETD_MAX_FRAME];
    for (;;) {
        uint32_t len = ring_read(&g_rings->rx, frame, sizeof(frame));
        if (!len)
            break;
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (!p)
            continue;
        uint32_t i;
        for (i = 0; i < len; i++)
            ((uint8_t *)p->payload)[i] = frame[i];
        if (g_netif.input(p, &g_netif) != ERR_OK)
            pbuf_free(p);
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    int64_t rh = a20_syscall6(A20_SYS_netd_attach, 0, 0, 0, 0, 0, 0);
    if (rh < 0)
        return 2;
    uint64_t base = 0;
    if (a20_vm_map((a20_handle_t)rh, NETD_VMO_PAGES * 4096, 0,
                   A20_PROT_READ | A20_PROT_WRITE, &base) < 0)
        return 2;
    a20_hdl_close((a20_handle_t)rh);
    extern void a20_netd_set_out(a20_handle_t h);
    a20_start_info_t *si = a20_get_start_info();
    a20_netd_set_out(si ? si->stdout_handle : A20_HANDLE_NULL);

    g_rings = (netd_rings_t *)(uintptr_t)base;
    g_rings->rx.slot_mask = NETD_RING_SLOTS - 1;
    g_rings->tx.slot_mask = NETD_RING_SLOTS - 1;
    __atomic_store_n(&g_rings->tx.head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rings->tx.tail, 0, __ATOMIC_RELAXED);

    lwip_init();

    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    if (!netif_add(&g_netif, &ip, &mask, &gw, NULL, netd_netif_init,
                   netif_input))
        return 3;
    netif_set_default(&g_netif);

    a20_netd_printf("netd: up ip=10.0.2.15\n");

    /* Probe the frame plane: send an ARP who-has 10.0.2.2 once. */
    {
        uint8_t arp[64] = {0};
        const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        uint32_t i;
        for (i = 0; i < 6; i++) { arp[i] = 0xff; arp[6 + i] = mac[i]; }
        arp[12] = 0x08; arp[13] = 0x06;
        arp[14] = 0x00; arp[15] = 0x01; arp[16] = 0x08; arp[17] = 0x00;
        arp[18] = 0x06; arp[19] = 0x04; arp[20] = 0x00; arp[21] = 0x01;
        for (i = 0; i < 6; i++) arp[22 + i] = mac[i];
        arp[28] = 10; arp[29] = 0; arp[30] = 2; arp[31] = 15;
        arp[38] = 10; arp[39] = 0; arp[40] = 2; arp[41] = 2;
        ring_write(&g_rings->tx, arp, 42);
        a20_netd_printf("netd: ARP probe sent tx.h=%u tx.t=%u rx.h=%u rx.t=%u\n",
                        (unsigned)g_rings->tx.head, (unsigned)g_rings->tx.tail,
                        (unsigned)g_rings->rx.head, (unsigned)g_rings->rx.tail);
    }

    for (;;) {
        netd_rx_drain();
        sys_check_timeouts();
        netif_poll(&g_netif);
        a20_thread_yield();
    }
}
