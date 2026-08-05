/*
 * A20OS service registry channel (docs/hybrid-kernel/02-mainstream-plan.md
 * M3): one well-known channel pair created at boot.  The client endpoint
 * is installed into every native task's start_info (service_registry);
 * the server endpoint is claimed by the supervisor (svcman) via
 * sys_a20_registry_claim and re-claimable after the owner dies.
 */
#include "core/types.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/ipc_internal.h"

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);
extern void a20_object_ref(void *object, uint16_t type);
extern void a20_object_release(void *object, uint16_t type);

static a20_channel_ep_t *g_registry_server;
static a20_channel_ep_t *g_registry_client;
static spinlock_t        g_registry_lock = SPINLOCK_INIT;
static int               g_registry_owner_pid;

static int registry_task_alive(int pid)
{
    task_t *t = proc_find_get(pid);
    if (!t)
        return 0;
    proc_put(t);
    return 1;
}

void a20_registry_init(void)
{
    a20_channel_ep_t *ep0 = a20_channel_create(0, NULL);
    if (!ep0) {
        klog(KLOG_ERR, "registry: channel create failed\n");
        return;
    }
    g_registry_server = ep0;
    g_registry_client = ep0->peer;
}

a20_channel_ep_t *a20_registry_client_ep(void)
{
    return g_registry_client;
}

/* Install the shared client endpoint into @ht with a fresh reference. */
int64_t a20_registry_install_client(struct a20_ht_internal *ht)
{
    if (!ht || !g_registry_client)
        return -1;
    a20_object_ref(g_registry_client, A20_OBJ_CHANNEL_ENDPOINT);
    return a20_handle_install(ht, g_registry_client,
                              A20_OBJ_CHANNEL_ENDPOINT,
                              A20_RIGHT_READ | A20_RIGHT_WRITE);
}

int64_t sys_a20_registry_claim(const a20_syscall_args_t *args)
{
    (void)args;
    if (!g_registry_server)
        return -A20_ERR_NOT_SUPPORTED;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    /* Ownership CAS without holding the registry lock across proc_lock:
     * the lock is leaf-level, the exit path clears ownership separately. */
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_registry_lock);
        int owner = g_registry_owner_pid;
        spin_unlock_irqrestore(&g_registry_lock, flags);
        if (owner > 0 && registry_task_alive(owner)) {
            klog(KLOG_WARN, "registry: already owned by pid=%d\n", owner);
            return -A20_ERR_ACCESS;
        }
        flags = spin_lock_irqsave(&g_registry_lock);
        if (g_registry_owner_pid == owner) {
            g_registry_owner_pid = cur->pid;
            spin_unlock_irqrestore(&g_registry_lock, flags);
            break;
        }
        spin_unlock_irqrestore(&g_registry_lock, flags);
    }

    a20_object_ref(g_registry_server, A20_OBJ_CHANNEL_ENDPOINT);
    int64_t h = a20_handle_install(ht, g_registry_server,
                                   A20_OBJ_CHANNEL_ENDPOINT,
                                   A20_RIGHT_READ | A20_RIGHT_WRITE);
    if (h < 0) {
        a20_object_release(g_registry_server, A20_OBJ_CHANNEL_ENDPOINT);
        uint64_t f2 = spin_lock_irqsave(&g_registry_lock);
        if (g_registry_owner_pid == cur->pid)
            g_registry_owner_pid = 0;
        spin_unlock_irqrestore(&g_registry_lock, f2);
        return h;
    }
    klog(KLOG_INFO, "registry: claimed by pid=%d\n", cur->pid);
    return h;
}

/* Called from the task-exit path: a dead supervisor drops its claim so a
 * successor can take over (the endpoint itself stays alive via refs). */
void a20_registry_task_exit(int pid)
{
    uint64_t flags = spin_lock_irqsave(&g_registry_lock);
    if (g_registry_owner_pid == pid)
        g_registry_owner_pid = 0;
    spin_unlock_irqrestore(&g_registry_lock, flags);
}
