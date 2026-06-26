#include "net/socket_internal.h"

int a20_socket_in_registry(net_socket_t *s)
{
    return s ? s->in_registry : 0;
}

void a20_socket_set_in_registry(net_socket_t *s, int value)
{
    if (s)
        s->in_registry = value;
}

int a20_socket_reg_idx(net_socket_t *s)
{
    return s ? s->reg_idx : -1;
}

void a20_socket_set_reg_idx(net_socket_t *s, int value)
{
    if (s)
        s->reg_idx = value;
}
