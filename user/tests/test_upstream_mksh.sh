echo UPSTREAM_SHELL_SOURCE_OK
printf 'one\ntwo\n' | cat
x=$(printf abc); test "$x" = abc || exit 9
echo UPSTREAM_SHELL_PIPE_OK
