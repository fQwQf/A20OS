/* Same real program on flat MCUs and MMU servers. Standard Native startup,
 * file I/O and two actual work threads; no host-written execution result. */
#define A20_NATIVE_FILE_TEST 1
#include "liba20rt/a20_string.h"
#include "liba20rt/crt0_a20.h"
#include "native-thread-contract.h"
int main(int argc, char **argv, char **envp) {
    a20_start_info_t *info = a20_get_start_info();
    if (!info || info->size != sizeof(*info) || info->version != 1 ||
        info->argc != 4 || info->envc != 2 || info->page_size != 4096 ||
        info->self_task == A20_HANDLE_NULL || argc != 4 || !argv || !envp ||
        !argv[0] || !argv[1] || !argv[2] || !argv[3] || argv[4] || !envp[0] ||
        !envp[1] || envp[2])
        return 71;
    if (a20_strcmp(argv[0], "spawn") || a20_strcmp(argv[1], "hello cluster") ||
        a20_strcmp(argv[2], "") || a20_strcmp(argv[3], "37") ||
        a20_strcmp(envp[0], "A20_SPAWN=1") || a20_strcmp(envp[1], "EMPTY="))
        return 72;
    if (native_runtime_contract())
        return 73;
    if (native_file_contract() || native_file_reopen(3))
        return 74;
    if (native_thread_contract())
        return 75;
    if (!native_balance_result())
        return 76;
    return 37;
}
