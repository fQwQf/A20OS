#include "core/arch.h"
#include "core/string.h"
#include "core/consts.h"

static int build_sibling_path(const char *exec_path, const char *suffix,
                              char *resolved, size_t resolved_size) {
    if (!exec_path || !suffix || !resolved || resolved_size == 0)
        return -ENOENT;

    int exec_len = (int)strlen(exec_path);
    int suffix_len = (int)strlen(suffix);
    for (int i = exec_len - 1; i >= 0; i--) {
        if (exec_path[i] != '/')
            continue;
        if ((size_t)(i + suffix_len + 1) >= resolved_size)
            continue;
        memcpy(resolved, exec_path, (size_t)i);
        strcpy(resolved + i, suffix);
        return 0;
    }
    return -ENOENT;
}

static int build_mount_root_path(const char *exec_path, const char *suffix,
                                 char *resolved, size_t resolved_size) {
    if (!exec_path || exec_path[0] != '/' || !suffix || suffix[0] != '/' ||
        !resolved || resolved_size == 0)
        return -ENOENT;

    int end = 1;
    while (exec_path[end] && exec_path[end] != '/')
        end++;

    if ((size_t)end + strlen(suffix) >= resolved_size)
        return -ENOENT;
    memcpy(resolved, exec_path, (size_t)end);
    strcpy(resolved + end, suffix);
    return 0;
}

int arch_resolve_interp_fallback(const char *exec_path, const char *interp_path,
                                 char *resolved, size_t resolved_size) {
    if (interp_path && strstr(interp_path, "ld-musl-riscv64.so.1") &&
        build_sibling_path(exec_path, "/lib/libc.so", resolved, resolved_size) == 0)
        return 0;

    if (interp_path && strstr(interp_path, "ld-linux-riscv64-lp64d.so.1")) {
        if (build_mount_root_path(exec_path, "/glibc/lib/ld-linux-riscv64-lp64d.so.1",
                                  resolved, resolved_size) == 0)
            return 0;
    }

    return -ENOENT;
}
