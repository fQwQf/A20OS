#ifndef _FDTABLE_H
#define _FDTABLE_H

#include <stdint.h>

int      __fd_table_init(void);
int      __fd_alloc(uint32_t handle);
uint32_t __fd_to_handle(int fd);
void     __fd_free(int fd);

#endif
