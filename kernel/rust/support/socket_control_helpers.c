#include "net/socket_internal.h"
#include "net/lwip_stack.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/timer.h"
#include "lwip/tcp.h"

uint64_t a20_socket_control_ticks_per_sec(void)
{
    return TICKS_PER_SEC;
}

int a20_socket_control_task_state(task_t *task)
{
    return task ? (int)task->state : 0;
}

void a20_socket_control_proc_make_ready(task_t *task)
{
    if (task)
        proc_make_ready(task);
}

int a20_socket_control_alg_is(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

uint64_t a20_socket_control_lwip_lock(void)
{
    return a20_lwip_lock();
}

void a20_socket_control_lwip_unlock(uint64_t flags)
{
    a20_lwip_unlock(flags);
}

void a20_socket_control_tcp_nagle_disable(struct tcp_pcb *pcb)
{
    tcp_nagle_disable(pcb);
}

void a20_socket_control_tcp_nagle_enable(struct tcp_pcb *pcb)
{
    tcp_nagle_enable(pcb);
}

void a20_socket_control_tcp_set_keep_idle_ms(struct tcp_pcb *pcb, uint32_t value_ms)
{
    if (pcb)
        pcb->keep_idle = value_ms;
}

void a20_socket_control_tcp_set_keep_intvl_ms(struct tcp_pcb *pcb, uint32_t value_ms)
{
    if (pcb)
        pcb->keep_intvl = value_ms;
}

void a20_socket_control_tcp_set_keep_cnt(struct tcp_pcb *pcb, uint32_t value)
{
    if (pcb)
        pcb->keep_cnt = (u32_t)value;
}

uint32_t a20_socket_control_tcp_get_keep_idle_ms(struct tcp_pcb *pcb)
{
    return pcb ? pcb->keep_idle : 0;
}

uint32_t a20_socket_control_tcp_get_keep_intvl_ms(struct tcp_pcb *pcb)
{
    return pcb ? pcb->keep_intvl : 0;
}

uint32_t a20_socket_control_tcp_get_keep_cnt(struct tcp_pcb *pcb)
{
    return pcb ? pcb->keep_cnt : 0;
}

void a20_socket_control_tcp_set_so_keepalive(struct tcp_pcb *pcb, int enabled)
{
    if (!pcb)
        return;
    if (enabled)
        pcb->so_options |= SOF_KEEPALIVE;
    else
        pcb->so_options &= ~SOF_KEEPALIVE;
}
