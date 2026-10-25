#include "core/bootargs.h"
#include "firmware/devicetree.h"

int fdt_extract_bootargs(const void *blob, size_t available, char *out, size_t capacity)
{
    if (out && capacity) out[0] = 0;
    fw_dt_t tree;
    int rc = fw_dt_init(&tree, blob, available);
    return rc ? rc : fw_dt_bootargs(&tree, out, capacity);
}
