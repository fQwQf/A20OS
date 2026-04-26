#include "core/arch.h"
#include "core/consts.h"

int arch_resolve_interp_fallback(const char *exec_path, const char *interp_path,
                                 char *resolved, size_t resolved_size) {
    (void)exec_path;
    (void)interp_path;
    (void)resolved;
    (void)resolved_size;
    return -ENOENT;
}
