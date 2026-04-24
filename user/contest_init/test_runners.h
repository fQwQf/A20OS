#ifndef TEST_RUNNERS_H
#define TEST_RUNNERS_H

int run_glibc_basic_test(const char *script_name, const char *script_dir);
int run_glibc_busybox_test(const char *script_name, const char *script_dir);
int run_glibc_lua_test(const char *script_name, const char *script_dir);
int run_glibc_libctest_test(const char *script_name, const char *script_dir);
int run_musl_basic_test(const char *script_name, const char *script_dir);
int run_musl_busybox_test(const char *script_name, const char *script_dir);
int run_musl_lua_test(const char *script_name, const char *script_dir);

#endif /* TEST_RUNNERS_H */
