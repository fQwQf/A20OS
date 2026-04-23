#ifndef TEST_COMMON_RUNNER_H
#define TEST_COMMON_RUNNER_H

/* Execute script via /bin/mksh with cwd set to script directory. */
int run_script_via_mksh(const char *script_path, const char *tag, const char *path_env);

#endif /* TEST_COMMON_RUNNER_H */
