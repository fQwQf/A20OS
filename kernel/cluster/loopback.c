/*
 * loopback transport (docs/cluster/04-transports.md §2).
 *
 * 语义: send = 把帧直接投递到目标虚拟节点的 a20_clx_rx_frame（拷贝，不共享
 * 缓冲），零丢包、保序。链路地址 = 4 字节虚拟节点号；本机节点固定使用
 * A20_CLX_LOOPBACK_SELF_ADDR，其他虚拟节点经 cluster_route(loopback) 注册。
 *
 * 测试钩子 (CONFIG_CLUSTER_TEST_HOOKS，默认关，CI 测试构建开):
 *   drop / delay / dup / corrupt / partition。注入走内核命令行
 *   "clxhooks=drop=10,delay=5,dup=1,corrupt=1,part=100"（bootargs 惯例，同
 *   CHTRACE_BOOTARG），04-§2 登记为 v0 的注入机制。无钩子构建下本文件不
 *   含任何故障注入代码路径。
 */
#include "cluster/clx_internal.h"
#include "cluster/loopback.h"

#include "core/bootargs.h"
#include "core/klog.h"
#include "core/random.h"
#include "mm/slab.h"
#include "proc/proc.h"

typedef struct clx_vnode {
    int      in_use;
    uint32_t addr;                     /* 4B virtual node number */
    a20_node_id_t node;
} clx_vnode_t;

#define CLX_LOOPBACK_VNODES (CLX_LIMIT_ROUTES + 2)
static clx_vnode_t g_clx_vnodes[CLX_LOOPBACK_VNODES];
static spinlock_t g_clx_vnode_lock = SPINLOCK_INIT;

/* ---- virtual node registry ------------------------------------------------ */

int a20_clx_loopback_vnode_add(const a20_node_id_t *node,
                               const uint8_t *next_hop, uint32_t nh_len)
{
    uint32_t addr;

    if (!node || !next_hop || nh_len != 4)
        return -1;
    addr = (uint32_t)next_hop[0] | ((uint32_t)next_hop[1] << 8) |
           ((uint32_t)next_hop[2] << 16) | ((uint32_t)next_hop[3] << 24);
    if (addr == A20_CLX_LOOPBACK_SELF_ADDR)
        return -1;

    uint64_t flags = spin_lock_irqsave(&g_clx_vnode_lock);
    for (uint32_t i = 0; i < CLX_LOOPBACK_VNODES; i++) {
        if (g_clx_vnodes[i].in_use &&
            memcmp(g_clx_vnodes[i].node.bytes, node->bytes, 16) == 0) {
            g_clx_vnodes[i].addr = addr;    /* re-register: update */
            spin_unlock_irqrestore(&g_clx_vnode_lock, flags);
            return 0;
        }
    }
    for (uint32_t i = 0; i < CLX_LOOPBACK_VNODES; i++) {
        if (g_clx_vnodes[i].in_use)
            continue;
        g_clx_vnodes[i].in_use = 1;
        g_clx_vnodes[i].addr = addr;
        g_clx_vnodes[i].node = *node;
        spin_unlock_irqrestore(&g_clx_vnode_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&g_clx_vnode_lock, flags);
    return -1;
}

int a20_clx_loopback_vnode_del(const a20_node_id_t *node)
{
    uint64_t flags = spin_lock_irqsave(&g_clx_vnode_lock);

    for (uint32_t i = 0; i < CLX_LOOPBACK_VNODES; i++) {
        if (g_clx_vnodes[i].in_use &&
            memcmp(g_clx_vnodes[i].node.bytes, node->bytes, 16) == 0) {
            g_clx_vnodes[i].in_use = 0;
            spin_unlock_irqrestore(&g_clx_vnode_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_clx_vnode_lock, flags);
    return -1;
}

int a20_clx_loopback_addr_node(uint32_t transport_id, const uint8_t *nh,
                               uint32_t nh_len, a20_node_id_t *out)
{
    uint32_t addr;
    a20_node_id_t self;
    uint64_t flags;
    int r = -1;

    if (transport_id != A20_CLX_TRANSPORT_LOOPBACK || !nh || nh_len != 4)
        return -1;
    addr = (uint32_t)nh[0] | ((uint32_t)nh[1] << 8) | ((uint32_t)nh[2] << 16) |
           ((uint32_t)nh[3] << 24);
    if (a20_clx_self_get(&self, NULL) < 0)
        return -1;
    if (addr == A20_CLX_LOOPBACK_SELF_ADDR) {
        *out = self;
        return 0;
    }
    flags = spin_lock_irqsave(&g_clx_vnode_lock);
    for (uint32_t i = 0; i < CLX_LOOPBACK_VNODES; i++) {
        if (g_clx_vnodes[i].in_use && g_clx_vnodes[i].addr == addr) {
            *out = g_clx_vnodes[i].node;
            r = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&g_clx_vnode_lock, flags);
    return r;
}

/* ---- test hooks ------------------------------------------------------------ */

#ifdef CONFIG_CLUSTER_TEST_HOOKS
typedef struct clx_hooks {
    int      on;
    uint32_t drop_pct;      /* 0-100 random frame loss */
    uint32_t delay_ms;      /* frames enter a delay queue */
    uint32_t dup;           /* send the frame twice */
    uint32_t corrupt;       /* flip one random payload bit */
    uint32_t partition_a;   /* vnode pair cut off */
    uint32_t partition_b;
} clx_hooks_t;

static clx_hooks_t g_clx_hooks;
static uint64_t g_clx_hook_rng;

/* hook decisions are pure: seeded LCG, deterministic per boot seed. */
static uint32_t hook_rand(void)
{
    g_clx_hook_rng =
        g_clx_hook_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g_clx_hook_rng >> 33);
}

static void hooks_parse(void)
{
    /* clxhooks=drop=10,delay=5,dup=1,corrupt=1,part=2.3
     * (decimal vnode numbers, kernel atoi; format registered in 04-§2). */
    const char *args = bootargs_get();
    const char *p;

    if (!args || !(p = strstr(args, "clxhooks=")))
        return;
    p += strlen("clxhooks=");
    while (*p && *p != ' ') {
        if (!strncmp(p, "drop=", 5)) {
            g_clx_hooks.drop_pct = (uint32_t)atoi(p + 5) % 101;
            p += 5;
        } else if (!strncmp(p, "delay=", 6)) {
            g_clx_hooks.delay_ms = (uint32_t)atoi(p + 6);
            p += 6;
        } else if (!strncmp(p, "dup=", 4)) {
            g_clx_hooks.dup = (uint32_t)atoi(p + 4);
            p += 4;
        } else if (!strncmp(p, "corrupt=", 8)) {
            g_clx_hooks.corrupt = (uint32_t)atoi(p + 8);
            p += 8;
        } else if (!strncmp(p, "part=", 5)) {
            const char *q = p + 5;
            g_clx_hooks.partition_a = (uint32_t)atoi(q);
            while (*q && *q != ',' && *q != '.')
                q++;
            if (*q == '.')
                g_clx_hooks.partition_b = (uint32_t)atoi(q + 1);
            p = q;
        } else {
            break;
        }
        while (*p && *p != ',' && *p != ' ')
            p++;
        if (*p == ',')
            p++;
    }
    g_clx_hooks.on = 1;
    printf("[CLX] loopback hooks: drop=%u%% delay=%ums dup=%u corrupt=%u "
           "part=%u.%u\n", g_clx_hooks.drop_pct, g_clx_hooks.delay_ms,
           g_clx_hooks.dup, g_clx_hooks.corrupt, g_clx_hooks.partition_a,
           g_clx_hooks.partition_b);
}

static int hook_partitioned(uint32_t dst_addr)
{
    clx_hooks_t *h = &g_clx_hooks;

    if (!h->on)
        return 0;
    return (dst_addr == h->partition_a &&
            h->partition_b == A20_CLX_LOOPBACK_SELF_ADDR) ||
           (dst_addr == h->partition_b &&
            h->partition_a == A20_CLX_LOOPBACK_SELF_ADDR);
}

static int hook_drop(void)
{
    clx_hooks_t *h = &g_clx_hooks;

    return h->on && h->drop_pct && (hook_rand() % 100u) < h->drop_pct;
}

static void hook_corrupt(uint8_t *frame, uint32_t len)
{
    clx_hooks_t *h = &g_clx_hooks;
    uint32_t byte;
    uint8_t bit;

    if (!h->on || !h->corrupt || len <= A20_CLX_HDR_LEN + A20_CLX_CRC_LEN)
        return;
    /* flip one random payload bit so csum_kind=1 receivers exercise the
     * CRC path (04-§2) */
    byte = A20_CLX_HDR_LEN + hook_rand() % (len - A20_CLX_HDR_LEN - A20_CLX_CRC_LEN);
    bit = (uint8_t)(hook_rand() % 8);
    frame[byte] ^= (uint8_t)(1u << bit);
}

static void loopback_deliver(const uint8_t *frame, uint32_t len);

static void hook_delay(const uint8_t *frame, uint32_t len)
{
    /* v0: delivery is deferred in the TX-thread caller, which is always a
     * cluster worker thread (never an interrupt).  A timer-queue version
     * lands with WA2's UDP work. */
    uint64_t until = timer_get_ticks() + MS_TO_TICKS(g_clx_hooks.delay_ms);

    while (timer_get_ticks() < until)
        proc_sleep_until(until);
    loopback_deliver(frame, len);
}
#endif /* CONFIG_CLUSTER_TEST_HOOKS */

/* ---- transport ------------------------------------------------------------- */

/* The wire-level source address of every frame this kernel emits onto a
 * loopback wire: the local node's virtual number (both wire ends live in
 * this kernel; 04-§2). */
static const uint8_t clx_loopback_self_nh[4] = {
    (uint8_t)(A20_CLX_LOOPBACK_SELF_ADDR & 0xFF),
    (uint8_t)((A20_CLX_LOOPBACK_SELF_ADDR >> 8) & 0xFF),
    (uint8_t)((A20_CLX_LOOPBACK_SELF_ADDR >> 16) & 0xFF),
    (uint8_t)((A20_CLX_LOOPBACK_SELF_ADDR >> 24) & 0xFF),
};

/* Copy, don't share (04-§2): the upcall queues its own copy again, but
 * hook paths may hand us borrowed buffers. */
static void loopback_deliver(const uint8_t *frame, uint32_t len)
{
    uint8_t *copy = kmalloc(len);

    if (!copy)
        return;
    memcpy(copy, frame, len);
    a20_clx_rx_frame(A20_CLX_TRANSPORT_LOOPBACK, clx_loopback_self_nh, 4,
                     copy, len);
    kfree(copy);
}

void a20_clx_loopback_init(void)
{
#ifdef CONFIG_CLUSTER_TEST_HOOKS
    g_clx_hook_rng = random_u64();
    hooks_parse();
#endif
}

int a20_clx_loopback_send(void *ctx, const uint8_t *next_hop,
                          uint32_t nh_len, const void *frame, uint32_t len)
{
    (void)ctx;
    if (!next_hop || nh_len != 4 || !frame || len < A20_CLX_HDR_LEN)
        return -A20_ERR_INVALID_ARGUMENT;
#ifdef CONFIG_CLUSTER_TEST_HOOKS
    uint32_t dst_addr = (uint32_t)next_hop[0] | ((uint32_t)next_hop[1] << 8) |
                        ((uint32_t)next_hop[2] << 16) |
                        ((uint32_t)next_hop[3] << 24);
    if (g_clx_hooks.on) {
        if (hook_partitioned(dst_addr) || hook_drop())
            return 0;          /* 04-§1: 传输允许丢帧 */
        if (g_clx_hooks.dup)
            loopback_deliver(frame, len);
        if (g_clx_hooks.corrupt) {
            uint8_t *scratch = kmalloc(len);
            if (!scratch)
                return -A20_ERR_NO_MEMORY;
            memcpy(scratch, frame, len);
            hook_corrupt(scratch, len);
            loopback_deliver(scratch, len);
            kfree(scratch);
            return 0;
        }
        if (g_clx_hooks.delay_ms) {
            hook_delay(frame, len);
            return 0;
        }
    }
#endif

    loopback_deliver(frame, len);
    return 0;
}
