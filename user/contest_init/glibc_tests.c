#include "common_runner.h"
#include "test_runners.h"

int run_glibc_basic_test(const char *script_name, const char *script_dir)
{
    return run_script_in_dir(script_name, script_dir, "TEST][glibc][basic");
}

int run_glibc_busybox_test(const char *script_name, const char *script_dir)
{
    return run_script_in_dir(script_name, script_dir, "TEST][glibc][busybox");
}
