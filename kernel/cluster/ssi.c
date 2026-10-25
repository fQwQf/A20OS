/* Optional serialized single-address-space execution over real TCP. The home
 * task remains the syscall/resource owner. A worker runs only user instructions
 * and returns all writable private pages before the home lease is released. */
#ifdef CONFIG_CLUSTER_SSI
#include "cluster/ssi.h"
#include "cluster/arch_state.h"
#include "uapi/a20/cluster_ssi.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/fault.h"
#include "mm/process_vm.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "net/socket.h"
#include "net/net_config.h"
#include "../net/socket_internal.h"
#include "sys/syscall.h"
#include "proc/signal.h"

struct ssi_page {
    uint64_t va;
    uint32_t flags, returned;
};
struct ssi_pages {
    struct ssi_page entries[64];
    uint32_t len, limit;
    uint64_t bytes, cap;
    uint32_t phase, reserved;
};
extern int a20_ssi_state_configure(uint32_t, uint32_t, uint32_t);
extern int a20_ssi_state_managed(uint32_t);
extern int a20_ssi_state_allowed(uint32_t, uint32_t);
extern uint64_t a20_ssi_state_begin(uint32_t, uint32_t);
extern int a20_ssi_state_finish(uint32_t, uint32_t, uint64_t);
extern void a20_ssi_state_forget(uint32_t);
extern int a20_ssi_pages_init(struct ssi_pages *, uint32_t, uint64_t);
extern int a20_ssi_pages_charge(struct ssi_pages *, uint64_t);
extern int a20_ssi_pages_grant(struct ssi_pages *, uint64_t, uint32_t);
extern int a20_ssi_pages_return(struct ssi_pages *, uint64_t);
extern int a20_ssi_pages_complete(const struct ssi_pages *);

#define SSI_MAGIC 0x31495353U /* SSI1, little endian, x86-64 only */
enum { SSI_START = 1, SSI_FAULT, SSI_PAGE, SSI_LIMIT, SSI_WRITE, SSI_DONE };
enum { SSI_SYSCALL = 1, SSI_QUANTUM, SSI_BAD_ACCESS };
struct ssi_header {
    uint32_t magic, type;
    uint64_t epoch, va;
    uint32_t flags, length;
};
struct ssi_start {
    uint8_t token[32];
    uint32_t pages, quantum, bytes, reserved;
    cluster_arch_state_t user;
};
_Static_assert(sizeof(struct ssi_pages) == 1056,
               "Rust SSI page transaction ABI");
_Static_assert(sizeof(struct ssi_header) == 32, "SSI1 stream header ABI");
_Static_assert(sizeof(cluster_arch_state_t) == 672, "SSI1 user state ABI");
_Static_assert(sizeof(struct ssi_start) == 720, "SSI1 start ABI");
struct ssi_session {
    vfile_t *file;
    net_socket_t *socket;
    task_t *task;
    uint64_t epoch, deadline, started, user_ticks;
    uint32_t quantum;
    struct ssi_pages pages;
    cluster_arch_state_t user;
    uint8_t buffer[4096];
};
static struct a20_ssi_config config;
static task_t *config_owner;
static vfile_t *listener_file;
static net_socket_t *listener_socket;
static struct ssi_session *remote;
static int worker_running;
static int worker_live;
static int configuring;

/* Admission Rust calls with an internal spin guard require local IRQ
 * exclusion on UP: another task must never preempt the holder and spin. */
static int configure_state(uint32_t root, uint32_t threads, uint32_t slices) {
    int enabled = arch_irqs_enabled();
    arch_local_irq_disable();
    int r = a20_ssi_state_configure(root, threads, slices);
    if (enabled)
        arch_local_irq_enable();
    return r;
}

int cluster_ssi_managed(const task_t *t) {
    return t && a20_ssi_state_managed(t->tgid);
}
int cluster_ssi_task_allowed(const task_t *t) {
    return !t || a20_ssi_state_allowed(t->tgid, t->pid);
}
int cluster_ssi_mm_busy(const task_t *t) {
    task_t *caller = proc_current();
    return t && caller && cluster_ssi_managed(t) &&
           !a20_ssi_state_allowed(t->tgid, caller->pid);
}

static void address(net_sockaddr_in_t *a) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = __builtin_bswap16(config.port);
    memcpy(&a->sin_addr, config.address, 4);
}
static vfile_t *take_fd(int fd, net_socket_t **socket) {
    if (fd < 0)
        return NULL;
    vfile_t *file = fdtable_get_current_file_ref(fd);
    vfs_close(fd);
    if (!file)
        return NULL;
    *socket = net_socket_from_vfile(file);
    if (!*socket) {
        vfs_put_file(file);
        return NULL;
    }
    int one = 1;
    (void)net_setsockopt_sock(*socket, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
    return file;
}
/* Every stream operation shares one absolute slice deadline, including short
 * reads/writes. Nonblocking sockets avoid unbounded connect/send waits. */
static int transfer(struct ssi_session *s, void *buffer, size_t size,
                    int send) {
    uint8_t *p = buffer;
    while (size) {
        if (timer_get_ticks() >= s->deadline)
            return -ETIMEDOUT;
        int n = send ? net_sendto_sock(s->socket, p, size,
                                       MSG_DONTWAIT | MSG_NOSIGNAL, NULL, 0)
                     : net_recvfrom_socket_meta(s->socket, p, size,
                                                MSG_DONTWAIT, NULL, NULL, NULL);
        if (n == -EAGAIN || n == -EINPROGRESS || (send && n == -ENOTCONN)) {
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(1));
            continue;
        }
        if (n <= 0)
            return n ? n : -EIO;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}
static int send_packet(struct ssi_session *s, uint32_t type, uint64_t va,
                       uint32_t flags, void *body, uint32_t size) {
    struct ssi_header h = {SSI_MAGIC, type, s->epoch, va, flags, size};
    int r = transfer(s, &h, sizeof(h), 1);
    return r < 0 || !size ? r : transfer(s, body, size, 1);
}
static int receive_header(struct ssi_session *s, struct ssi_header *h) {
    int r = transfer(s, h, sizeof(*h), 0);
    if (r < 0)
        return r;
    return h->magic == SSI_MAGIC && h->epoch == s->epoch && h->length <= 4096
               ? 0
               : -EIO;
}
static int safe_mapping(const mm_seg_t *v) {
    return v && !(v->vm_flags &
                  (VM_SHARED | VM_PFNMAP | VM_SYSV_SHM | VM_VMO | VM_GUARD));
}
static int safe_address_space(task_t *t) {
    uint64_t irq = spin_lock_irqsave(&t->mm->lock);
    int ok = 1;
    for (mm_seg_t *v = t->mm->mmap; v; v = v->next)
        if (!safe_mapping(v) && !(v->vm_flags & VM_GUARD)) {
            ok = 0;
            break;
        }
    spin_unlock_irqrestore(&t->mm->lock, irq);
    return ok;
}
/* Serve a genuine user fault from the authoritative home MM. All sibling
 * threads are excluded by the scheduler lease, so mappings cannot change. */
static int serve_page(struct ssi_session *s, struct ssi_header *h) {
    if (h->length || (h->va & 4095) || h->va >= USER_VA_LIMIT || !h->flags ||
        (h->flags & ~7U))
        return -EIO;
    task_t *t = proc_current();
    uint64_t irq = spin_lock_irqsave(&t->mm->lock);
    mm_seg_t *v = mm_seg_find(t->mm, h->va);
    uint32_t prot = safe_mapping(v) ? (v->vm_flags & 7U) : 0;
    spin_unlock_irqrestore(&t->mm->lock, irq);
    if ((prot & h->flags) != h->flags)
        return -EFAULT;
    if (s->pages.len >= s->pages.limit ||
        a20_ssi_pages_charge(&s->pages, 4096 + ((prot & 2) ? 4096 : 0)) < 0)
        return send_packet(s, SSI_LIMIT, h->va, 0, NULL, 0);
    int r = a20_ssi_pages_grant(&s->pages, h->va, prot);
    if (r < 0)
        return r;
    enum mm_fault_access access = (h->flags & 2)   ? MM_FAULT_ACCESS_WRITE
                                  : (h->flags & 4) ? MM_FAULT_ACCESS_EXEC
                                                   : MM_FAULT_ACCESS_READ;
    (void)handle_demand_fault_access(t, h->va, access);
    if (process_vm_read_kernel(t, (void *)(uintptr_t)h->va, s->buffer, 4096) !=
        4096)
        return -EFAULT;
    return send_packet(s, SSI_PAGE, h->va, prot, s->buffer, 4096);
}
static void close_session(struct ssi_session *s) {
    if (s->file) {
        net_shutdown_sock(s->socket, 2);
        vfs_put_file(s->file);
    }
    s->file = NULL;
}
static void finish_remote(trap_context_t *ctx, uint32_t reason)
    __attribute__((noreturn));
static void finish_remote(trap_context_t *ctx, uint32_t reason) {
    struct ssi_session *s = remote;
    arch_cluster_state_capture(&s->user, ctx);
    arch_local_irq_enable();
    int r = 0;
    for (uint32_t i = 0; i < s->pages.len && r == 0; i++) {
        struct ssi_page *p = &s->pages.entries[i];
        if (!(p->flags & 2))
            continue;
        if (process_vm_read_kernel(proc_current(), (void *)(uintptr_t)p->va,
                                   s->buffer, 4096) != 4096) {
            r = -EFAULT;
            break;
        }
        r = send_packet(s, SSI_WRITE, p->va, 0, s->buffer, 4096);
    }
    if (r == 0)
        r = send_packet(s, SSI_DONE, 0, reason, &s->user, sizeof(s->user));
    kinfo("[SSI] worker slice epoch=%lu pid=%d pages=%u user-ticks=%lu "
          "reason=%u result=%d\n",
          s->epoch, proc_current()->pid, s->pages.len,
          proc_current()->utime_ticks - s->user_ticks, reason, r);
    proc_exit(r < 0 ? 1 : 0);
}
static void remote_fault(trap_context_t *ctx, uint64_t cause) {
    struct ssi_session *s = remote;
    uint64_t va = arch_read_tval() & ~4095ULL;
    uint32_t access = cause == CAUSE_STORE_PAGE_FAULT  ? 2
                      : cause == CAUSE_INSN_PAGE_FAULT ? 4
                                                       : 1;
    arch_local_irq_enable();
    int r = send_packet(s, SSI_FAULT, va, access, NULL, 0);
    struct ssi_header h;
    if (r == 0)
        r = receive_header(s, &h);
    if (r < 0)
        finish_remote(ctx, SSI_BAD_ACCESS);
    if (h.type == SSI_LIMIT && !h.length)
        finish_remote(ctx, SSI_QUANTUM);
    if (h.type != SSI_PAGE || h.va != va || h.length != 4096 ||
        (h.flags & access) != access || (h.flags & ~7U) ||
        a20_ssi_pages_charge(&s->pages, 4096 + ((h.flags & 2) ? 4096 : 0)) <
            0 ||
        a20_ssi_pages_grant(&s->pages, va, h.flags) < 0)
        finish_remote(ctx, SSI_BAD_ACCESS);
    if (transfer(s, s->buffer, 4096, 0) < 0)
        finish_remote(ctx, SSI_BAD_ACCESS);
    /* Construct ordinary private anon VMAs through the MM API. No raw PTE
     * writes, shared frames, physical addresses or remote kernel pointers. */
    if (mm_mmap(s->task->mm, va, 4096, 3, 0x32) != va ||
        user_prepare_write(s->task, va) < 0 ||
        process_vm_write_kernel(s->task, (void *)(uintptr_t)va, s->buffer,
                                4096) != 4096 ||
        mm_mprotect(s->task->mm, va, 4096, h.flags) < 0)
        finish_remote(ctx, SSI_BAD_ACCESS);
    arch_local_irq_disable();
}
int cluster_ssi_intercept(trap_context_t *ctx) {
    if (!remote || remote->task != proc_current())
        return 0;
    proc_current()->trap_ctx = ctx;
    ARCH_TRAP_FAST_RETURN_DISARM(ctx);
    uint64_t cause = arch_read_cause();
    if (cause & CAUSE_INTR_MASK)
        return 0;
    cause &= CAUSE_CODE_MASK;
    if (cause == CAUSE_ECALL_U) {
        arch_advance_syscall_epc(ctx);
        finish_remote(ctx, SSI_SYSCALL);
    }
    if (cause == CAUSE_LOAD_PAGE_FAULT || cause == CAUSE_STORE_PAGE_FAULT ||
        cause == CAUSE_INSN_PAGE_FAULT) {
        remote_fault(ctx, cause);
        return 1;
    }
    finish_remote(ctx, SSI_BAD_ACCESS);
}
static void worker(void) {
    fdtable_init(proc_current());
    kinfo("[SSI] worker listening port=%u pages=%u\n", config.port,
          config.max_pages);
    while (__atomic_load_n(&worker_running, __ATOMIC_ACQUIRE)) {
        net_socket_t *socket = NULL;
        int fd = net_accept_sock(listener_socket, NULL, NULL, SOCK_NONBLOCK);
        if (fd == -EAGAIN) {
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10));
            continue;
        }
        kinfo("[SSI] accept result=%d\n", fd);
        vfile_t *file = take_fd(fd, &socket);
        if (!file)
            break;
        struct ssi_session *s = kcalloc(1, sizeof(*s));
        if (!s) {
            vfs_put_file(file);
            break;
        }
        s->file = file;
        s->socket = socket;
        s->started = timer_get_ticks();
        s->deadline = s->started + MS_TO_TICKS(config.timeout_ms);
        struct ssi_header h;
        struct ssi_start *start = kmalloc(sizeof(*start));
        int r = start ? transfer(s, &h, sizeof(h), 0) : -ENOMEM;
        if (r == 0 &&
            (h.magic != SSI_MAGIC || h.type != SSI_START || !h.epoch || h.va ||
             h.flags || h.length != sizeof(*start)))
            r = -EIO;
        if (r == 0)
            r = transfer(s, start, sizeof(*start), 0);
        if (r == 0 &&
            (memcmp(start->token, config.token, 32) || start->reserved ||
             !start->pages || start->pages > config.max_pages ||
             !start->quantum || start->quantum > config.quantum_ticks ||
             start->bytes > config.max_bytes || !arch_cluster_state_valid(&start->user)))
            r = -EINVAL;
        if (r == 0) {
            s->epoch = h.epoch;
            s->quantum = start->quantum;
            r = a20_ssi_pages_init(&s->pages, start->pages, start->bytes);
        }
        if (r == 0) {
            /* Allocate the deferred task before allocating its root. This
             * avoids ambiguous image ownership on the generic image helper's
             * early task/PID/stack allocation failures. No task is runnable
             * until the root and both saved context tokens are installed. */
            uint64_t entry, stack, tls;
            arch_cluster_state_user_entry(&start->user, &entry, &stack, &tls);
            int pid = proc_alloc_user_image(entry, stack, NULL, NULL, 0,
                                            stack, 0, tls, 1);
            if (pid < 0) {
                r = pid;
            } else {
                task_t *t = proc_find_get(pid);
                pt_root_t *pgdir = t && t->mm ? pt_create() : NULL;
                if (!pgdir) {
                    r = -ENOMEM;
                    proc_free_pid(pid);
                } else {
                    pt_map_kernel(pgdir);
                    t->pgdir = t->mm->pgdir = pgdir;
                    uint64_t as = mm_address_space_token(t->mm);
                    TRAP_CTX_KScratch0(t->trap_ctx) = as;
                    task_context_t *base =
                        arch_task_context_base(t->kstack_base, 0, t->trap_ctx);
                    TASK_CTX_PAGE_TABLE(base) = as;
                    arch_setup_signal_trampoline(t->mm);
                    s->task = t;
                    t->abi_mode = 0;
                    arch_cluster_state_restore(&start->user, t, t->trap_ctx, 0);
                    s->user_ticks = t->utime_ticks;
                    remote = s;
                    proc_publish_deferred_task(t);
                    int status;
                    (void)proc_wait4(pid, &status, 0);
                    remote = NULL;
                }
                proc_put(t);
            }
        }
        kfree(start);
        close_session(s);
        kfree(s);
    }
    vfs_put_file(listener_file);
    listener_file = NULL;
    listener_socket = NULL;
    __atomic_store_n(&worker_running, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&worker_live, 0, __ATOMIC_RELEASE);
    proc_exit(0);
}
static int64_t configure_inner(uint64_t address_ptr) {
    task_t *t = proc_current();
    struct a20_ssi_config c;
    if (!t || t->cred.euid != 0)
        return -EPERM;
    if (copy_from_user(&c, (void *)(uintptr_t)address_ptr, sizeof(c)) < 0)
        return -EFAULT;
    if (c.version != A20_SSI_VERSION || c.role > A20_SSI_WORKER || c.reserved ||
        c.shared_memory > 1 || c.automatic_threads > 1)
        return -EINVAL;
    if (config_owner && config_owner != t)
        return -EBUSY;
    if (c.role == A20_SSI_OFF) {
        if (remote)
            return -EBUSY;
        (void)configure_state(0, 0, 1);
        __atomic_store_n(&worker_running, 0, __ATOMIC_RELEASE);
        config_owner = NULL;
        config.role = 0;
        return 0;
    }
    if (worker_live || config_owner || t->tgid != t->pid || t->sched_policy ||
        !t->mm || refcount_read(&t->mm->refcount) != 1 || !c.port ||
        c.max_pages < 1 || c.max_pages > 64 || c.max_threads < 1 ||
        c.max_threads > 16 || !c.max_slices || c.max_slices > 100000 ||
        !c.quantum_ticks || c.quantum_ticks > 100 || c.timeout_ms < 100 ||
        c.timeout_ms > 60000 || c.max_bytes < 8192 || c.max_bytes > 1048576)
        return -EINVAL;
    uint8_t token = 0;
    for (unsigned i = 0; i < 32; i++)
        token |= c.token[i];
    if (!token)
        return -EINVAL;
    config = c;
    if (c.role == A20_SSI_WORKER) {
        if (!c.shared_memory || !c.automatic_threads)
            return -EINVAL;
        if (g_a20_tcp_path != A20_TCP_PATH_LWIP) {
            kinfo("[SSI] worker requires a20.tcpmode=lwip\n");
            return -EOPNOTSUPP;
        }
        listener_file =
            take_fd(net_socket_create(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0),
                    &listener_socket);
        if (!listener_file)
            return -EIO;
        net_sockaddr_in_t addr;
        address(&addr);
        int r = net_bind_sock(listener_socket, &addr, sizeof(addr));
        if (r == 0)
            r = net_listen_sock(listener_socket, 1);
        if (r < 0) {
            vfs_put_file(listener_file);
            listener_file = NULL;
            return r;
        }
        worker_running = 1;
        worker_live = 1;
        r = proc_alloc_detached(worker);
        if (r < 0) {
            worker_running = worker_live = 0;
            vfs_put_file(listener_file);
            listener_file = NULL;
            return r;
        }
    } else {
        (void)configure_state(t->tgid, c.max_threads, c.max_slices);
    }
    config_owner = t;
    return 0;
}
int64_t cluster_ssi_configure(uint64_t address_ptr) {
    if (__atomic_exchange_n(&configuring, 1, __ATOMIC_ACQUIRE))
        return -EBUSY;
    int64_t r = configure_inner(address_ptr);
    __atomic_store_n(&configuring, 0, __ATOMIC_RELEASE);
    return r;
}
void cluster_ssi_exit(task_t *t) {
    if (t == config_owner) {
        config_owner = NULL;
        config.role = 0;
        (void)configure_state(0, 0, 1);
        __atomic_store_n(&worker_running, 0, __ATOMIC_RELEASE);
    }
    int enabled = arch_irqs_enabled();
    arch_local_irq_disable();
    if (cluster_ssi_managed(t) && t->pid != t->tgid)
        kinfo("[SSI] home thread released tid=%d\n", t->pid);
    a20_ssi_state_forget(t->pid);
    if (enabled)
        arch_local_irq_enable();
}

/* Called with IRQs disabled at the final user return boundary. The lease is
 * acquired before enabling IRQs, and remains held through validated writeback.
 * Only the home proxy may be scheduled from this process during the slice. */
void cluster_ssi_before_user(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (remote && remote->task == t) {
        if (t->utime_ticks - remote->user_ticks >= remote->quantum ||
            timer_get_ticks() >= remote->deadline)
            finish_remote(ctx, SSI_QUANTUM);
        return;
    }
    if (!t || config.role != A20_SSI_HOME || !config.shared_memory ||
        !config.automatic_threads || !t->mm || t->abi_mode || t->sched_policy ||
        t->rseq_area || t->exit_pending || !cluster_ssi_managed(t))
        return;
    uint64_t epoch = a20_ssi_state_begin(t->tgid, t->pid);
    if (!epoch)
        return;
    if (!safe_address_space(t)) {
        a20_ssi_state_finish(t->tgid, t->pid, epoch);
        return;
    }
    struct ssi_session *s = kcalloc(1, sizeof(*s));
    uint8_t *staged = kmalloc((size_t)config.max_pages * 4096);
    struct ssi_start *start = kmalloc(sizeof(*start));
    if (!s || !staged || !start) {
        kfree(s);
        kfree(staged);
        kfree(start);
        a20_ssi_state_finish(t->tgid, t->pid, epoch);
        return;
    }
    s->epoch = epoch;
    s->started = timer_get_ticks();
    s->deadline = s->started + MS_TO_TICKS(config.timeout_ms);
    a20_ssi_pages_init(&s->pages, config.max_pages, config.max_bytes);
    memset(start, 0, sizeof(*start));
    memcpy(start->token, config.token, 32);
    start->pages = config.max_pages;
    start->quantum = config.quantum_ticks;
    start->bytes = config.max_bytes;
    arch_cluster_state_capture(&start->user, ctx);
    arch_local_irq_enable();
    s->file = take_fd(
        net_socket_create(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), &s->socket);
    int r = s->file ? 0 : -EIO;
    net_sockaddr_in_t addr;
    address(&addr);
    if (r == 0) {
        r = net_connect_sock(s->socket, &addr, sizeof(addr));
        kinfo("[SSI] connect epoch=%lu result=%d\n", epoch, r);
        if (r == -EINPROGRESS)
            r = 0;
    }
    if (r == 0)
        r = send_packet(s, SSI_START, 0, 0, start, sizeof(*start));
    uint32_t reason = 0;
    while (r == 0 && !reason) {
        struct ssi_header h;
        r = receive_header(s, &h);
        if (r < 0)
            break;
        if (h.type == SSI_FAULT)
            r = serve_page(s, &h);
        else if (h.type == SSI_WRITE && h.length == 4096 && !h.flags) {
            int index = a20_ssi_pages_return(&s->pages, h.va);
            r = index < 0 ? index : transfer(s, staged + index * 4096, 4096, 0);
        } else if (h.type == SSI_DONE && !h.va && h.length == sizeof(s->user) &&
                   (h.flags == SSI_SYSCALL || h.flags == SSI_QUANTUM)) {
            r = transfer(s, &s->user, sizeof(s->user), 0);
            if (r == 0 && (!arch_cluster_state_valid(&s->user) ||
                           !a20_ssi_pages_complete(&s->pages)))
                r = -EIO;
            if (r == 0)
                reason = h.flags;
        } else
            r = -EIO;
    }
    if (r == 0) {
        for (uint32_t i = 0; i < s->pages.len; i++) {
            struct ssi_page *p = &s->pages.entries[i];
            if (!(p->flags & 2))
                continue;
            if (process_vm_write_kernel(t, (void *)(uintptr_t)p->va,
                                        staged + i * 4096, 4096) != 4096) {
                r = -EFAULT;
                break;
            }
        }
    }
    close_session(s);
    kinfo("[SSI] home slice epoch=%lu tid=%d pages=%u bytes=%lu reason=%u "
          "result=%d\n",
          epoch, t->pid, s->pages.len, s->pages.bytes, reason, r);
    arch_local_irq_disable();
    if (r == 0)
        arch_cluster_state_restore(&s->user, t, ctx, 1);
    kfree(start);
    kfree(staged);
    kfree(s);
    a20_ssi_state_finish(t->tgid, t->pid, epoch);
    if (r < 0) {
        /* Never replay uncertain execution or release partial memory as a
         * usable process. Group exit is published before siblings run. */
        proc_exit_group(-SIGKILL);
    }
    if (reason == SSI_SYSCALL) {
        ARCH_TRAP_FAST_RETURN_DISARM(ctx);
        arch_syscall_dispatch_enter();
        syscall_dispatch(ctx);
        arch_syscall_dispatch_leave();
        proc_check_exit_pending();
    }
}
#endif
