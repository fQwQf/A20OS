#ifndef _FDTABLE_H
#define _FDTABLE_H

#include <stdint.h>

struct __fd_entry {
    uint32_t handle;
    uint32_t fd_flags;
    uint32_t open_flags;
};

int      __fd_table_init(void);
int      __fd_alloc(uint32_t handle);
uint32_t __fd_to_handle(int fd);
void     __fd_free(int fd);
int      __fd_set(int fd, uint32_t handle);

int      __fd_alloc_from(uint32_t handle, int min_fd);
void     __fd_set_handle(int fd, uint32_t handle);
void     __fd_set_open_flags(int fd, uint32_t flags);
uint32_t __fd_get_open_flags(int fd);
void     __fd_set_fd_flags(int fd, uint32_t flags);
uint32_t __fd_get_fd_flags(int fd);

#endif
