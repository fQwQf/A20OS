#ifndef _ABI_LINUX_STAT_ABI_H
#define _ABI_LINUX_STAT_ABI_H

#include "fs/vfs.h"

void arch_copy_kstat_to_user(void *st, const kstat_t *kst);
int arch_copy_statfs64_to_user(void *buf, const kstatfs_t *kst);

#endif /* _ABI_LINUX_STAT_ABI_H */
