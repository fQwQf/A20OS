#include "net/socket_internal.h"
#include "fs/file.h"
#include "mm/objcache.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "core/stdio.h"
#include "drivers/net/virtio_net.h"
#include "net/lwip_stack.h"

spinlock_t g_net_lock = SPINLOCK_INIT;
static obj_cache_t g_net_socket_cache = OBJ_CACHE_INIT("net_socket", net_socket_t, 128);

net_socket_t *a20_socket_core_alloc_raw(void)
{
    net_socket_t *s = (net_socket_t *)obj_cache_alloc_zero(&g_net_socket_cache);
    if (s) {
        s->bpf_prog_fd = -1;
        s->ipv6_checksum_offset = -1;
        s->reg_idx = -1;
    }
    return s;
}

void a20_socket_core_free_raw(net_socket_t *s)
{
    if (s)
        obj_cache_free(&g_net_socket_cache, s);
}

task_t *a20_socket_core_proc_current(void)
{
    return proc_current();
}

void a20_socket_core_proc_set_wake_time(task_t *task, uint64_t when)
{
    if (task)
        proc_set_wake_time(task, when);
}

int a20_socket_core_task_state(task_t *task)
{
    return task ? (int)task->state : 0;
}

void a20_socket_core_task_set_state(task_t *task, int state)
{
    if (task)
        task->state = (proc_state_t)state;
}

int a20_socket_core_task_has_unblocked_signal_impl(task_t *t)
{
    if (!t || !t->signals)
        return 0;
    if (__atomic_load_n(&t->exit_pending, __ATOMIC_ACQUIRE))
        return 1;
    signal_state_t *ss = (signal_state_t *)t->signals;
    return ((ss->pending | t->thread_pending) & ~t->sig_blocked) != 0;
}

uint64_t a20_socket_core_net_wait_ticks(void)
{
    return NET_WAIT_TICKS;
}

int a20_socket_core_current_euid(void)
{
    task_t *cur = proc_current();
    return cur ? (int)cur->cred.euid : -1;
}

int a20_socket_core_current_has_cap_net_raw(void)
{
    task_t *cur = proc_current();
    if (!cur)
        return 0;
    return (cur->cred.cap_effective & (1ULL << CAP_NET_RAW)) != 0;
}

void a20_socket_core_sched(void)
{
    sched();
}

int a20_socket_core_virtio_net_init_once(void)
{
    return virtio_net_init();
}

void a20_socket_core_lwip_init(void)
{
    a20_lwip_init();
}

void a20_socket_core_lwip_poll(void)
{
    a20_lwip_poll();
}

int a20_socket_core_lwip_format_status(char *buf, size_t bufsz)
{
    return a20_lwip_format_status(buf, bufsz);
}

int a20_socket_core_append_status(char *buf, size_t bufsz, int used, int bound, int queued)
{
    return snprintf(buf, bufsz,
                    "syscall-sockets: open=%d bound=%d queued=%d\n",
                    used, bound, queued);
}

void a20_socket_core_log_init(void)
{
    printf("[NET] socket layer initialized\n");
}
