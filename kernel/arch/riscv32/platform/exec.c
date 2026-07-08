#include "core/arch.h"
#include "core/string.h"
#include "core/consts.h"
#include "fs/pathutil.h"

int arch_resolve_interp_fallback(const char *exec_path, const char *interp_path, char *resolved, size_t resolved_size) {
    if (interp_path && strstr(interp_path, "ld-musl-riscv32.so.1") &&
        path_build_sibling(exec_path, "/lib/libc.so", resolved, resolved_size) == 0)
        return 0;
    if (interp_path && strstr(interp_path, "ld-linux-riscv32-ilp32d.so.1")) {
        static const char *glibc_paths[] = {
            "/lib/ld-linux-riscv32-ilp32d.so.1",
            "/lib32/ld-linux-riscv32-ilp32d.so.1",
        };
        for (int i = 0; i < (int)(sizeof(glibc_paths) / sizeof(glibc_paths[0])); i++) {
            size_t len = strlen(glibc_paths[i]);
            if (len < resolved_size) {
                memcpy(resolved, glibc_paths[i], len + 1);
                return 0;
            }
        }
    }
    return -ENOENT;
}
