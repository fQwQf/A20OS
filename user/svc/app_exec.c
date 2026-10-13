/* Replace the owner-managed Native root task with an existing Linux ABI
 * program. The external task handle and object identity survive exec. */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../../kernel/include/uapi/a20/abi_features.h"
static void note(const char *s) {
    a20_start_info_t *si = a20_get_start_info();
    if (si && si->stderr_handle != A20_HANDLE_NULL)
        a20_hdl_write_buf(si->stderr_handle, s, a20_strlen(s), 0);
}
int main(int argc, char **argv, char **envp) {
    if (argc < 2 || !argv[1] || argv[1][0] != '/') {
        note("usage: app-exec /absolute/program [argument...]\n");
        return 64;
    }
    if (a20_syscall6(A20_SYS_feature_test, A20_ABI_FEATURE_LINUX_EXEC, 0, 0, 0,
                     0, 0) < 0) {
        note("APP-EXEC: Linux exec is unavailable in this execution domain\n");
        return 126;
    }
    a20_status_t status =
        a20_syscall6(A20_SYS_execve, (uintptr_t)argv[1], (uintptr_t)(argv + 1),
                     (uintptr_t)envp, 0, 0, 0);
    (void)status; /* Success resumes the replacement image, never this code. */
    note("APP-EXEC: image replacement failed\n");
    return 127;
}
