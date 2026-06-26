#include "net/socket_internal.h"
#include "net/lwip_stack.h"
#include "mm/objcache.h"
#include "proc/signal.h"
#include "sys/bpf.h"

static obj_cache_t g_rust_net_msg_cache = OBJ_CACHE_INIT("net_msg", net_msg_t, 16);

net_msg_t *net_msg_alloc(void)
{
    return (net_msg_t *)obj_cache_alloc_zero(&g_rust_net_msg_cache);
}

void net_msg_free(net_msg_t *m)
{
    obj_cache_free(&g_rust_net_msg_cache, m);
}

void *a20_net_socket_from_file(int gfd)
{
    return net_socket_from_file(gfd);
}

void a20_net_lwip_poll(void)
{
    a20_lwip_poll();
}

void a20_net_tcp_recved(void *s, size_t len)
{
    if (s)
        net_tcp_recved((net_socket_t *)s, len);
}

void *a20_proc_current_task(void)
{
    return proc_current();
}

void a20_sched_yield(void)
{
    sched();
}

void a20_proc_make_ready_task(void *task)
{
    if (task)
        proc_make_ready((task_t *)task);
}

void a20_proc_set_wake_time_task(void *task, uint64_t wake_time)
{
    if (task)
        proc_set_wake_time((task_t *)task, wake_time);
}

int a20_task_state_value(void *task)
{
    return task ? (int)((task_t *)task)->state : 0;
}

void a20_task_set_state_value(void *task, int state)
{
    if (task)
        ((task_t *)task)->state = (proc_state_t)state;
}

int a20_task_has_unblocked_signal(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    if (__atomic_load_n(&t->exit_pending, __ATOMIC_ACQUIRE))
        return 1;
    signal_state_t *ss = (signal_state_t *)t->signals;
    return ((ss->pending | t->thread_pending) & ~t->sig_blocked) != 0;
}

int a20_net_socket_wait_expired(void *socket_ptr, uint64_t start, int for_write)
{
    net_socket_t *s = (net_socket_t *)socket_ptr;
    if (!s)
        return 0;
    uint64_t timeout = for_write ? s->send_timeout_ticks : s->recv_timeout_ticks;
    return timeout && (int64_t)(timer_get_ticks() - (start + timeout)) >= 0;
}

void a20_bpf_run_socket_filter(int fd)
{
    bpf_run_socket_filter(fd);
}

uint64_t a20_net_wait_ticks_value(void)
{
    return NET_WAIT_TICKS;
}

uint64_t a20_ms_to_ticks_value(uint64_t ms)
{
    return MS_TO_TICKS(ms);
}
