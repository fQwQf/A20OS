#include "common_runner.h"
#include "test_runners.h"

int run_musl_basic_test(const char *script_name, const char *script_dir)
{
    return run_script_in_dir(script_name, script_dir, "TEST][musl][basic");
}

int run_musl_busybox_test(const char *script_name, const char *script_dir)
{
    return run_script_in_dir(script_name, script_dir, "TEST][musl][busybox");
}

int run_musl_lua_test(const char *script_name, const char *script_dir) {
    return run_script_in_dir(script_name, script_dir, "TEST][musl][busybox");
}

int run_musl_ltp_test(const char *script_name, const char *script_dir) {
    return run_script_in_dir(script_name, script_dir, "TEST][musl][ltp");
}