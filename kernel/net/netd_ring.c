/*
 * netd frame plane: shared frame rings between the kernel (virtio-net)
 * and the userspace netd service (lwIP).
 *
 * The kernel allocates one VMO holding an RX ring (kernel -> netd) and a
 * TX ring (netd -> kernel); netd attaches through A20_SYS_netd_attach and
 * maps it.  With the frame plane active the virtio-net RX path delivers
 * frames into the RX ring instead of the in-kernel lwIP stack, and the TX
 * path drains the TX ring into the NIC.
 */
#include "core/types.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/vmo.h"
#include "mm/frame.h"
#include "ipc/ipc.h"
#include "ipc/objstats.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/objects.h"
#include "abi/native/handle_table.h"
#include "abi/native/syscall_entry.h"
#include "proc/proc.h"
#include "net/netd_proto.h"
#include "core/bootargs.h"
#include "core/string.h"

static struct vmo *g_netd_ring_vmo;
static netd_rings_t *g_netd_rings;
static int g_netd_enabled;

void netd_ring_init(void)
{
    /* Enable the frame plane only on explicit request: with it active the
     * NIC frames go to netd instead of the in-kernel lwIP stack. */
    const char *ba = bootargs_get();
    if (!ba || !strstr(ba, "netd"))
        return;
    g_netd_ring_vmo = vmo_create(VMO_ANONYMOUS, NETD_VMO_PAGES * PAGE_SIZE, 0);
    if (!g_netd_ring_vmo)
        return;
    /* Materialize every frame-plane page before touching the ring layout:
     * the rings span ~32 pages, and pfn_to_virt of an unmaterialized page
     * would alias arbitrary physical memory. */
    for (uint32_t i = 0; i < NETD_VMO_PAGES; i++) {
        pfn_t pfn = vmo_get_page(g_netd_ring_vmo, i);
        if (pfn == PFN_NONE)
            return;
    }
    pfn_t pfn = vmo_get_page(g_netd_ring_vmo, 0);
    g_netd_rings = (netd_rings_t *)pfn_to_virt(pfn);
    memset(g_netd_rings, 0, sizeof(*g_netd_rings));
    g_netd_rings->rx.slot_mask = NETD_RING_SLOTS - 1;
    g_netd_rings->tx.slot_mask = NETD_RING_SLOTS - 1;
    g_netd_enabled = 1;
    printf("[NETD] frame plane ready: rings=%p\n", (void *)g_netd_rings);
}

int netd_enabled(void) { return g_netd_enabled; }

/* Deliver a received frame into the RX ring. Returns 0 on success. */
int netd_rx_frame(const void *data, uint32_t len)
{
    if (!g_netd_enabled || !g_netd_rings)
        return -1;
    netd_frame_ring_t *r = &g_netd_rings->rx;
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if (head - tail >= NETD_RING_SLOTS)
        return -1;
    if (len > NETD_MAX_FRAME)
        len = NETD_MAX_FRAME;
    uint32_t idx = head & r->slot_mask;
    uint32_t off = idx * (4 + NETD_MAX_FRAME);
    *(uint32_t *)(r->data + off) = len;
    memcpy(r->data + off + 4, data, len);
    __atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&r->doorbell, 1, __ATOMIC_RELEASE);
    return 0;
}

/* Pull a frame from the TX ring for the NIC. Returns length or 0. */
uint32_t netd_tx_frame(void *out, uint32_t max)
{
    if (!g_netd_enabled || !g_netd_rings)
        return 0;
    netd_frame_ring_t *r = &g_netd_rings->tx;
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    if (head == tail)
        return 0;
    uint32_t idx = tail & r->slot_mask;
    uint32_t off = idx * (4 + NETD_MAX_FRAME);
    uint32_t len = *(const uint32_t *)(r->data + off);
    if (len > max || len > NETD_MAX_FRAME)
        return 0;
    memcpy(out, r->data + off + 4, len);
    __atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELEASE);
    return len;
}

int64_t sys_a20_netd_attach(const a20_syscall_args_t *args)
{
    (void)args;
    if (!g_netd_ring_vmo)
        return -A20_ERR_NOT_SUPPORTED;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    vmo_ref(g_netd_ring_vmo);
    int64_t h = a20_handle_install(ht, g_netd_ring_vmo, A20_OBJ_MEMORY,
                                   A20_RIGHT_READ | A20_RIGHT_WRITE |
                                   A20_RIGHT_MAP | A20_RIGHT_DUP);
    if (h < 0) {
        vmo_release(g_netd_ring_vmo);
        return h;
    }
    return h;
}
