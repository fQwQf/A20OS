#include "net/socket_internal.h"

#include "core/string.h"

net_socket_t *g_sockets[NET_MAX_SOCKETS];

void net_socket_registry_init(void) {
    memset(g_sockets, 0, sizeof(g_sockets));
}

int net_register_socket_locked(net_socket_t *s) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!g_sockets[i]) {
            g_sockets[i] = s;
            return 0;
        }
    }
    return -ENFILE;
}

void net_unregister_socket_locked(net_socket_t *s) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (g_sockets[i] == s) {
            g_sockets[i] = NULL;
            break;
        }
    }
}

int net_socket_is_valid_locked(net_socket_t *s) {
    if (!s) return 0;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (g_sockets[i] == s)
            return 1;
    }
    return 0;
}
