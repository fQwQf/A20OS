#ifndef _ABI_LINUX_STAT_ABI_H
#define _ABI_LINUX_STAT_ABI_H

#include "fs/vfs.h"

void arch_copy_kstat_to_user(void *st, const kstat_t *kst);
int arch_copy_statfs64_to_user(void *buf, int fs_type);

#endif /* _ABI_LINUX_STAT_ABI_H */
