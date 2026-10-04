/*
 * A20OS Linux ABI — channel bridge.
 *
 * Exposes the kernel's unified channel IPC (the same a20_channel_ep_t
 * objects the Native ABI references through its handle table) to Linux ABI
 * programs through file descriptors.  This is the "one mechanism, two thin
 * wrappers" boundary: a Linux program can create a channel pair or open the
 * well-known service-registry client endpoint and use plain read()/write()
 * on it, so the native service layer (registry, svcman, ...) is consumable
 * from both ABIs.
 */
#include "syscall_impl.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "core/lock.h"
#include "fs/fdtable.h"
#include "ipc/envelope.h"
#include "ipc/ipc.h"
#include "ipc/channel_fd.h"
#include "proc/proc.h"
#include "mm/slab.h"
#include "hyp/hyp.h"
#include "hyp/hyp_vcpu.h"

/* a20_channel_fd_install() returns a task-local fd.  Resolve its global fd
 * before exposing it to userspace so the bridge follows the same acquisition
 * contract as the other Linux fd-creation paths. */
static int mediate_channel_fd(int fd)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) {
        fdtable_close_current(fd);
        return (int)gfd;
    }
    env_kind_register((int)gfd, A20_OBJ_CHANNEL_ENDPOINT);
    if (!env_active(proc_current()))
        return 0;

    uint64_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT;
    int r = env_mediate_acquire((uint8_t)A20_OBJ_CHANNEL_ENDPOINT, rights,
                                (int)gfd);
    if (r)
        fdtable_close_current(fd);
    return r;
}

/*
 * SYS_a20_channel_pair(int fds[2]) — create a channel pair and return both
 * endpoints as file descriptors (socketpair-style).  Messages written on one
 * end are received whole on the other (SOCK_SEQPACKET semantics).
 */
int64_t sys_a20_channel_pair(const linux_syscall_args_t *args)
{
    int *fds = (int *)(uintptr_t)args->arg[0];
    if (!fds)
        return -EFAULT;

    a20_channel_ep_t *ep0 = a20_channel_create(0, NULL);
    if (!ep0)
        return -ENOMEM;
    a20_channel_ep_t *ep1 = ep0->peer;

    int a = a20_channel_fd_install(ep0, O_RDWR);
    if (a < 0) {
        a20_channel_ep_release(ep1);
        return a;
    }
    int mr = mediate_channel_fd(a);
    if (mr) {
        a20_channel_ep_release(ep1);
        return mr;
    }
    int b = a20_channel_fd_install(ep1, O_RDWR);
    if (b < 0) {
        fdtable_close_current(a);
        return b;
    }
    mr = mediate_channel_fd(b);
    if (mr) {
        fdtable_close_current(a);
        return mr;
    }

    int out[2] = { a, b };
    if (copy_to_user(fds, out, sizeof(out)) < 0) {
        fdtable_close_current(a);
        fdtable_close_current(b);
        return -EFAULT;
    }
    return 0;
}

/*
 * SYS_a20_registry_client(void) — return the well-known service-registry
 * client endpoint (the same one installed in every Native task's start_info)
 * as a file descriptor.  A Linux program uses read()/write() on it to issue
 * registry RPCs to the supervisor (svcman).
 */
int64_t sys_a20_registry_client_fd(const linux_syscall_args_t *args)
{
    (void)args;
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    extern a20_channel_ep_t *a20_registry_client_ep(void);
    a20_channel_ep_t *ep = a20_registry_client_ep();
    if (!ep)
        return -EOPNOTSUPP;
    a20_object_ref(ep, A20_OBJ_CHANNEL_ENDPOINT);
    int fd = a20_channel_fd_install(ep, O_RDWR);
    if (fd < 0)
        return fd;
    int mr = mediate_channel_fd(fd);
    return mr ? mr : fd;
#else
    /* Native ABI support is not built into this kernel image. */
    return -EOPNOTSUPP;
#endif
}

/*
 * SYS_a20_envelope_create(policy) — create an envelope from a policy struct
 * and return its id (for a20_envelope_enter / revoke / stats).
 */
int64_t sys_a20_envelope_create(const linux_syscall_args_t *args)
{
    extern int64_t env_create(const a20_env_policy_t *policy, uint32_t flags);
    return env_create((const a20_env_policy_t *)(uintptr_t)args->arg[0],
                      (uint32_t)args->arg[1]);
}

/*
 * SYS_a20_envelope_enter(env_id) — attach the CALLING process to an
 * envelope.  Intended to run in a forked child before execve: the trusted
 * supervisor creates the envelope and forks; untrusted code only ever runs
 * after enter().  Monotone: a second enter fails.
 */
int64_t sys_a20_envelope_enter(const linux_syscall_args_t *args)
{
    extern int env_enter(int64_t env_id);
    return env_enter((int64_t)args->arg[0]);
}

/*
 * SYS_a20_envelope_revoke(env_id) — active revocation: mark expired and,
 * with KILL_ON_EXPIRE, kill attached tasks.  Owner or root only.
 */
int64_t sys_a20_envelope_revoke(const linux_syscall_args_t *args)
{
    extern int env_revoke(int64_t env_id);
    return env_revoke((int64_t)args->arg[0]);
}

/*
 * SYS_a20_envelope_stats(env_id, out) — read mediation counters.
 */
int64_t sys_a20_envelope_stats(const linux_syscall_args_t *args)
{
    extern int env_stats(int64_t env_id, a20_env_stats_t *out);
    return env_stats((int64_t)args->arg[0],
                     (a20_env_stats_t *)(uintptr_t)args->arg[1]);
}

/*
 * SYS_a20_envelope_audit(out) -- E8 runtime invariant audit: re-verifies
 * TypeAllowed / RightsSubCap / budget bounds / attachment consistency
 * across every live envelope (docs/research/08 §2.4).
 */
int64_t sys_a20_envelope_audit(const linux_syscall_args_t *args)
{
    extern int64_t env_audit(struct a20_env_audit *out);
    return env_audit((struct a20_env_audit *)(uintptr_t)args->arg[0]);
}

/* ---------------------------------------------------------------------------
 * Hypervisor VM/vcpu bridge.
 *
 * The Native ABI exposes the vcpu slice as five calls in its 0x0F00 class
 * (kernel/abi/native/sys_native_hyp.c), where a VM and a vcpu are objects in
 * the caller's handle table.  A Linux program cannot reach that path: the
 * handle table is compiled only into ABI=native/both images
 * (Makefile: ABI_SRCS) and is allocated only for a task execed through the
 * Native startup protocol (kernel/proc/exec.c), so an ABI=linux task has
 * task->a20_ht == NULL.  These five calls are the same operations over the
 * same kernel objects (kernel/hyp/, hyp_vcpu.h), addressed by a slot index in
 * the bounded table below -- the shape the envelope control plane already
 * uses for its env_id (kernel/ipc/envelope.c).  The slot is tagged with the
 * owning pid, so one task can never name another's VM.
 *
 * Ownership is reaped by hyp_bridge_task_exit() from proc_exit().  That is not
 * only a leak fix: proc_pid_alloc() recycles a dead task's pid, so without the
 * hook a later task can be handed the same pid and every slot keyed on it --
 * and then load into, run and tear down a VM belonging to a task that no
 * longer exists.  "Enforced, not assumed" (hyp_vcpu.h) only holds because the
 * key cannot outlive its task.
 * ------------------------------------------------------------------------- */

#define HYP_BRIDGE_SLOTS 16
#define HYP_LOAD_MAX     (16 * 1024 * 1024)

enum { HYP_SLOT_FREE = 0, HYP_SLOT_VM, HYP_SLOT_VCPU };

struct hyp_bridge_slot {
    int   used;
    int   owner;
    int   kind;
    void *obj;
};

static struct hyp_bridge_slot g_hyp_slots[HYP_BRIDGE_SLOTS];
static spinlock_t g_hyp_slots_lock = SPINLOCK_INIT;

static int64_t hyp_slot_install(int owner, int kind, void *obj)
{
    int64_t slot = -1;
    uint64_t flags = spin_lock_irqsave(&g_hyp_slots_lock);
    for (int i = 0; i < HYP_BRIDGE_SLOTS; i++) {
        if (g_hyp_slots[i].used)
            continue;
        g_hyp_slots[i].used  = 1;
        g_hyp_slots[i].owner = owner;
        g_hyp_slots[i].kind  = kind;
        g_hyp_slots[i].obj   = obj;
        slot = i;
        break;
    }
    spin_unlock_irqrestore(&g_hyp_slots_lock, flags);
    return slot;
}

/* Resolve a userspace id to its object, or NULL when the slot is free, holds
 * another task's object, or holds the other kind. */
static void *hyp_slot_get(int owner, int kind, int64_t id)
{
    if (id < 0 || id >= HYP_BRIDGE_SLOTS)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&g_hyp_slots_lock);
    void *obj = NULL;
    if (g_hyp_slots[id].used && g_hyp_slots[id].owner == owner &&
        g_hyp_slots[id].kind == kind)
        obj = g_hyp_slots[id].obj;
    spin_unlock_irqrestore(&g_hyp_slots_lock, flags);
    return obj;
}

/* Detach and return the object; the caller owns the reference from here. */
static void *hyp_slot_take(int owner, int kind, int64_t id)
{
    if (id < 0 || id >= HYP_BRIDGE_SLOTS)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&g_hyp_slots_lock);
    void *obj = NULL;
    if (g_hyp_slots[id].used && g_hyp_slots[id].owner == owner &&
        g_hyp_slots[id].kind == kind) {
        obj = g_hyp_slots[id].obj;
        g_hyp_slots[id].used = 0;
        g_hyp_slots[id].obj = NULL;
    }
    spin_unlock_irqrestore(&g_hyp_slots_lock, flags);
    return obj;
}

static hyp_vm_t *hyp_bridge_vm(int64_t id)
{
    hyp_vm_t *vm = (hyp_vm_t *)hyp_slot_get(proc_current()->pid, HYP_SLOT_VM, id);
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return NULL;
    return vm;
}

static hyp_vcpu_t *hyp_bridge_vcpu(int64_t id)
{
    hyp_vcpu_t *vcpu =
        (hyp_vcpu_t *)hyp_slot_get(proc_current()->pid, HYP_SLOT_VCPU, id);
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC)
        return NULL;
    return vcpu;
}

/* SYS_hyp_vm_create(mem_size) -> vm id >= 0, or a negative errno. */
int64_t sys_hyp_vm_create(const linux_syscall_args_t *args)
{
    uint64_t mem_size = args->arg[0];
    if (mem_size == 0)
        return -EINVAL;
    if (!hyp_supported())
        return -EOPNOTSUPP;

    hyp_vm_t *vm = hyp_vm_create(mem_size);
    if (!vm)
        return -ENOMEM;

    int64_t id = hyp_slot_install(proc_current()->pid, HYP_SLOT_VM, vm);
    if (id < 0) {
        hyp_vm_put(vm);
        return -ENOSPC;
    }
    return id;
}

/* Overwrite a staging page through a volatile pointer.  This tree has no
 * explicit_bzero(), and a plain memset() on a buffer that is dead immediately
 * after is a store the compiler is free to delete; the guest image that passes
 * through this page is exactly the kind of thing that must not survive into
 * whatever slab allocation lands next. */
static void hyp_scrub_page(void *page)
{
    volatile uint8_t *q = (volatile uint8_t *)page;
    for (uint64_t i = 0; i < PAGE_SIZE; i++)
        q[i] = 0;
}

/*
 * SYS_hyp_vm_load(vm, gpa, buf, len) -> 0
 *
 * hyp_vm_load() wants a kernel pointer, so the payload is staged one page at a
 * time; gpa must be page-aligned because the frozen contract takes whole
 * stage-2 pages (the tail chunk is short and the kernel rounds it up).
 *
 * The staging page is kmalloc'd rather than automatic: 4 KiB of array is 6% of
 * a 64 KiB task stack, spent on the deepest path of a syscall that is already
 * several frames deep.  It is scrubbed before release on every path out,
 * because a slab page outlives this call and is then handed to whatever
 * allocates next.
 */
int64_t sys_hyp_vm_load(const linux_syscall_args_t *args)
{
    uint64_t gpa = args->arg[1];
    const char *ubuf = (const char *)(uintptr_t)args->arg[2];
    uint64_t len = args->arg[3];
    if (!ubuf)
        return -EFAULT;
    if (len == 0 || len > HYP_LOAD_MAX)
        return -EINVAL;
    if (gpa & (PAGE_SIZE - 1))
        return -EINVAL;

    hyp_vm_t *vm = hyp_bridge_vm((int64_t)args->arg[0]);
    if (!vm)
        return -EBADF;

    char *page = kmalloc(PAGE_SIZE);
    if (!page)
        return -ENOMEM;

    int64_t rc = 0;
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done;
        if (chunk > PAGE_SIZE)
            chunk = PAGE_SIZE;
        if (copy_from_user(page, ubuf + done, chunk) < 0) {
            rc = -EFAULT;
            break;
        }
        int lr = hyp_vm_load(vm, gpa + done, page, chunk);
        if (lr == -ENOMEM) {
            rc = -ENOMEM;
            break;
        }
        if (lr < 0) {
            rc = -EINVAL;
            break;
        }
        done += chunk;
    }

    hyp_scrub_page(page);
    kfree(page);
    return rc;
}

/* SYS_hyp_vcpu_create(vm, entry_gpa) -> vcpu id >= 0, or a negative errno. */
int64_t sys_hyp_vcpu_create(const linux_syscall_args_t *args)
{
    if (!hyp_supported())
        return -EOPNOTSUPP;

    hyp_vm_t *vm = hyp_bridge_vm((int64_t)args->arg[0]);
    if (!vm)
        return -EBADF;

    hyp_vcpu_t *vcpu = hyp_vcpu_create(vm, args->arg[1]);
    if (!vcpu)
        return -ENOMEM;

    int64_t id = hyp_slot_install(proc_current()->pid, HYP_SLOT_VCPU, vcpu);
    if (id < 0) {
        hyp_vcpu_put(vcpu);
        return -ENOSPC;
    }
    return id;
}

/*
 * SYS_hyp_vcpu_run(vcpu) -> exit reason >= 0, or a negative errno.
 *
 * The exit reason is returned rather than the kernel C API's "0, see
 * vcpu->exit" because a caller has to be able to tell a guest shutdown from a
 * guest fault; hyp_exit_reason_t is part of the frozen contract
 * (HYP_EXIT_SHUTDOWN == 1).
 */
int64_t sys_hyp_vcpu_run(const linux_syscall_args_t *args)
{
    if (!hyp_supported())
        return -EOPNOTSUPP;

    hyp_vcpu_t *vcpu = hyp_bridge_vcpu((int64_t)args->arg[0]);
    if (!vcpu)
        return -EBADF;

    if (hyp_vcpu_run(vcpu) < 0)
        return -EIO;
    return (int64_t)vcpu->exit;
}

/* SYS_hyp_vm_destroy(vm) -> 0.  A vcpu keeps its VM alive through the
 * reference hyp_vcpu_create() took, so only the caller's own reference goes. */
int64_t sys_hyp_vm_destroy(const linux_syscall_args_t *args)
{
    hyp_vm_t *vm = (hyp_vm_t *)hyp_slot_take(proc_current()->pid,
                                             HYP_SLOT_VM,
                                             (int64_t)args->arg[0]);
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return -EBADF;
    hyp_vm_put(vm);
    return 0;
}

/*
 * Task-exit reaper for the slot table above, called from proc_exit() alongside
 * udriver_task_cleanup()/udisk_task_exit().  Two things depend on it:
 *
 *  - the leak.  SYS_hyp_vm_destroy is optional for a task that simply exits,
 *    and there is no vcpu-destroy in the five-call set at all, so without this
 *    the VM (and the guest frames hyp_vm_load lent it) and the vcpu's VM
 *    reference are never dropped.
 *
 *  - the ownership guarantee.  proc_pid_alloc() recycles pids from a bitmap,
 *    so a dead task's pid comes back.  A slot keyed on that pid would then be
 *    reachable by an unrelated task, which could load into, run and destroy
 *    another task's guest.  Clearing the key before the pid is reusable is
 *    what makes the per-task check above sound rather than merely present.
 *
 * Detach under the lock, drop the references outside it: hyp_vcpu_put() and
 * hyp_vm_put() free, and hyp_vm_put() tears down a stage-2 page table walk.
 * Vcpus go first, because each one holds a reference on its VM.
 */
void hyp_bridge_task_exit(int pid)
{
    hyp_vcpu_t *drop_vcpu[HYP_BRIDGE_SLOTS];
    hyp_vm_t    *drop_vm[HYP_BRIDGE_SLOTS];
    int n_vcpu = 0, n_vm = 0;

    uint64_t flags = spin_lock_irqsave(&g_hyp_slots_lock);
    for (int i = 0; i < HYP_BRIDGE_SLOTS; i++) {
        if (!g_hyp_slots[i].used || g_hyp_slots[i].owner != pid)
            continue;
        if (g_hyp_slots[i].kind == HYP_SLOT_VCPU && n_vcpu < HYP_BRIDGE_SLOTS)
            drop_vcpu[n_vcpu++] = (hyp_vcpu_t *)g_hyp_slots[i].obj;
        else if (g_hyp_slots[i].kind == HYP_SLOT_VM && n_vm < HYP_BRIDGE_SLOTS)
            drop_vm[n_vm++] = (hyp_vm_t *)g_hyp_slots[i].obj;
        g_hyp_slots[i].used = 0;
        g_hyp_slots[i].obj = NULL;
    }
    spin_unlock_irqrestore(&g_hyp_slots_lock, flags);

    for (int i = 0; i < n_vcpu; i++)
        hyp_vcpu_put(drop_vcpu[i]);
    for (int i = 0; i < n_vm; i++)
        hyp_vm_put(drop_vm[i]);
}
