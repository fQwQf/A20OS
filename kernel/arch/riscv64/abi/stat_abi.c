#include "arch/common/stat_abi_64le.h"

void arch_copy_kstat_to_user(void *st, const kstat_t *kst)
{
    arch_copy_kstat64le_to_user(st, kst);
}

int arch_copy_statfs64_to_user(void *buf, const kstatfs_t *kst)
{
    return arch_copy_statfs64_64le_to_user(buf, kst);
}
