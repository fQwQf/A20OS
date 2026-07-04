#ifndef _ABI_LINUX_STAT_ABI_H
#define _ABI_LINUX_STAT_ABI_H

#include "fs/vfs.h"

/*
 * Architecture-specific copy of kernel kstat_t into the Linux userspace
 * struct stat layout.  The generic Linux struct stat layout used by
 * riscv64/aarch64/loongarch64 places st_mode at offset 16 and st_nlink
 * at offset 20; x86_64 places st_nlink at offset 16 and st_mode at
 * offset 24.  Each architecture that needs a different layout provides
 * a strong implementation of this function.
 */
void arch_copy_kstat_to_user(void *st, const kstat_t *kst);

#endif /* _ABI_LINUX_STAT_ABI_H */
