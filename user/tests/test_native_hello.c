#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    if (si && si->stdout_handle != A20_HANDLE_NULL) {
        a20_hdl_write_buf(si->stdout_handle,
                          "Hello from A20 Native SDK!\n", 27, (void *)0);
    }

    return 0;
}
