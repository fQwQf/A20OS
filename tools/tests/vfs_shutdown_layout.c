#include "fs/vfs/shutdown.h"

int vfs_shutdown_layout_translation_unit(void)
{
    return (int)sizeof(vfs_shutdown_ops_t);
}
