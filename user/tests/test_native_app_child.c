#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
static volatile uint32_t limit = 1000, work;
int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    uint32_t sum = 0;
    for (unsigned i = 1; i <= limit; i++)
        sum += i;
    if (sum != 500500)
        return 91;
    a20_start_info_t *si = a20_get_start_info();
    if (!si || argc != 1 || !argv || !argv[0])
        return 92;
    a20_path_open_args_t file = {.dir = si->root_dir,
                                 .path = (uint64_t)(uintptr_t)argv[0],
                                 .path_len = (uint32_t)a20_strlen(argv[0]),
                                 .rights = A20_RIGHT_READ};
    if (a20_path_open(&file) < 0)
        return 93;
    if (a20_hdl_close(file.out_handle) < 0)
        return 94;
    if (si && si->stdout_handle != A20_HANDLE_NULL)
        a20_hdl_write_buf(si->stdout_handle, "APP-WORK: PASS sum=500500\n", 26,
                          0);
#ifdef APP_EXIT
    return APP_EXIT;
#else
    for (;;) {
        uint32_t next = work;
        for (unsigned i = 1; i <= limit; i++)
            next = (next * 33) ^ i;
        work = next;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
#endif
}
