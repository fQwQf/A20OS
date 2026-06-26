#include "net/socket_internal.h"

void a20_ip_addr_set_ip4_u32(ip_addr_t *ip, uint32_t val)
{
    ip_addr_set_ip4_u32(ip, val);
}

uint32_t a20_ip_addr_get_ip4_u32(const ip_addr_t *ip)
{
    return ip_addr_get_ip4_u32(ip);
}

int a20_ip_is_v4(const ip_addr_t *ip)
{
    return IP_IS_V4(ip);
}
