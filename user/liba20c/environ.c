/*
 * A20OS liba20c — environment variable support.
 */
#include <stdlib.h>
#include <string.h>

char **environ = NULL;

void __environ_init(char **envp)
{
    environ = envp;
}

char *getenv(const char *name)
{
    if (!environ || !name) return NULL;
    size_t nlen = strlen(name);
    for (char **ep = environ; *ep; ep++) {
        if (strncmp(*ep, name, nlen) == 0 && (*ep)[nlen] == '=')
            return *ep + nlen + 1;
    }
    return NULL;
}
