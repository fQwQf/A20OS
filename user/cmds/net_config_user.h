#ifndef _NET_CONFIG_USER_H
#define _NET_CONFIG_USER_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int net_config_read_dns0(uint32_t *out_addr)
{
    if (!out_addr)
        return -1;

    FILE *f = fopen("/proc/net/config", "r");
    if (!f)
        return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "dns0=", 5) != 0)
            continue;

        const char *val = line + 5;
        unsigned a, b, c, d;
        if (sscanf(val, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            *out_addr = (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

#endif /* _NET_CONFIG_USER_H */
