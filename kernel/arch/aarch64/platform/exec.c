#include "core/arch.h"
#include "core/string.h"
#include "core/consts.h"

static int copy_fixed_path(const char *path, char *resolved, size_t resolved_size) {
    if (!path || !resolved || resolved_size == 0)
        return -ENOENT;
    if (strlen(path) >= resolved_size)
        return -ENOENT;
    strcpy(resolved, path);
    return 0;
}

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

int arch_resolve_interp_fallback(const char *exec_path, const char *interp_path,
                                 char *resolved, size_t resolved_size) {
    if (interp_path && strstr(interp_path, "ld-musl-aarch64.so.1") &&
        build_sibling_path(exec_path, "/lib/libc.so", resolved, resolved_size) == 0)
        return 0;

    if (interp_path && strstr(interp_path, "ld-linux-aarch64.so.1")) {
        static const char *const glibc_candidates[] = {
            "/test/glibc/lib/ld-linux-aarch64.so.1",
            "/testla/glibc/lib/ld-linux-aarch64.so.1",
            "/testrv/glibc/lib/ld-linux-aarch64.so.1",
            NULL,
        };
        for (int i = 0; glibc_candidates[i]; i++) {
            if (copy_fixed_path(glibc_candidates[i], resolved, resolved_size) == 0)
                return 0;
        }
    }

    return -ENOENT;
}
