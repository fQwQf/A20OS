#include "core/bootargs.h"
#include "core/string.h"

#define BOOTARGS_MAX 1024

static char g_bootargs[BOOTARGS_MAX];
static int g_bootargs_ready;

/* Weak default: no command line on this architecture. */
__attribute__((weak)) const char *arch_bootargs_get(void)
{
    return NULL;
}

void bootargs_init(void)
{
    if (g_bootargs_ready)
        return;

    const char *arch = arch_bootargs_get();
    if (arch && arch[0]) {
        strncpy(g_bootargs, arch, sizeof(g_bootargs) - 1);
        g_bootargs[sizeof(g_bootargs) - 1] = '\0';
    } else {
        g_bootargs[0] = '\0';
    }
    g_bootargs_ready = 1;
}

const char *bootargs_get(void)
{
    if (!g_bootargs_ready)
        bootargs_init();
    return g_bootargs[0] ? g_bootargs : NULL;
}
