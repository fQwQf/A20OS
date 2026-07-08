#include "arch/common/stat_abi_64le.h"

int arch_copy_statfs64_to_user(void *buf, int fs_type)
{
    return arch_copy_statfs64_64le_to_user(buf, fs_type);
}
