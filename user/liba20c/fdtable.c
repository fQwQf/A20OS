/*
 * A20OS liba20c — fd ↔ handle mapping layer.
 * Maps POSIX fd (0-1023) to A20 handle indices.
 */
#include <stdint.h>
#include "../liba20rt/a20_syscall.h"

#define FD_TABLE_INIT  32
#define FD_TABLE_MAX   1024

static uint32_t *fd_table;
static int fd_table_cap;

extern void *__bare_alloc(size_t n);

int __fd_table_init(void)
{
    fd_table_cap = FD_TABLE_INIT;
    fd_table = (uint32_t *)__bare_alloc(fd_table_cap * sizeof(uint32_t));
    if (!fd_table) return -1;
    for (int i = 0; i < fd_table_cap; i++)
        fd_table[i] = 0xFFFFFFFF;
    return 0;
}

int __fd_alloc(uint32_t handle)
{
    for (int i = 0; i < fd_table_cap; i++) {
        if (fd_table[i] == 0xFFFFFFFF) {
            fd_table[i] = handle;
            return i;
        }
    }
    if (fd_table_cap < FD_TABLE_MAX) {
        int new_cap = fd_table_cap * 2;
        if (new_cap > FD_TABLE_MAX) new_cap = FD_TABLE_MAX;
        uint32_t *new_tbl = (uint32_t *)__bare_alloc(new_cap * sizeof(uint32_t));
        if (!new_tbl) return -1;
        for (int i = 0; i < fd_table_cap; i++)
            new_tbl[i] = fd_table[i];
        for (int i = fd_table_cap; i < new_cap; i++)
            new_tbl[i] = 0xFFFFFFFF;
        fd_table = new_tbl;
        fd_table_cap = new_cap;
        fd_table[fd_table_cap / 2] = handle;
        return fd_table_cap / 2;
    }
    return -1;
}

uint32_t __fd_to_handle(int fd)
{
    if (fd < 0 || fd >= fd_table_cap) return 0xFFFFFFFF;
    return fd_table[fd];
}

void __fd_free(int fd)
{
    if (fd >= 0 && fd < fd_table_cap)
        fd_table[fd] = 0xFFFFFFFF;
}
