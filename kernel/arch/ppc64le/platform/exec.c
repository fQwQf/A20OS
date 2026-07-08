#include "core/arch.h"
#include "core/string.h"
#include "core/consts.h"

int arch_resolve_interp_fallback(const char *exec_path, const char *interp_path,
                                 char *resolved, size_t resolved_size) {
    (void)exec_path;

    if (interp_path && strstr(interp_path, "ld64.so.2")) {
        static const char *glibc_paths[] = {
            "/lib64/ld64.so.2",
            "/lib/ld64.so.2",
        };
        for (int i = 0; i < (int)(sizeof(glibc_paths) / sizeof(glibc_paths[0])); i++) {
            size_t len = strlen(glibc_paths[i]);
            if (len < resolved_size) {
                memcpy(resolved, glibc_paths[i], len + 1);
                return 0;
            }
        }
    }

    if (interp_path && strstr(interp_path, "ld-musl-powerpc64le.so.1")) {
        static const char *musl_path = "/lib/libc.so";
        size_t len = strlen(musl_path);
        if (len < resolved_size) {
            memcpy(resolved, musl_path, len + 1);
            return 0;
        }
    }

    return -ENOENT;
}
