#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    uint32_t cpu;
    a20_system_info_t system_info;
    if (a20_thread_get_cpu(&cpu) != A20_OK)
        return 1;
    if (a20_system_info(&system_info) != A20_OK ||
        system_info.online_cpus == 0 || cpu >= system_info.configured_cpus)
        return 2;
    if (si && si->stdout_handle != A20_HANDLE_NULL) {
        a20_hdl_write_buf(si->stdout_handle,
                          "Hello from A20 Native SDK!\n", 27, (void *)0);
    }

    return 0;
}
