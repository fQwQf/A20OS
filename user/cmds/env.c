#include <stdio.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    for (char **e = environ; e && *e; e++)
        printf("%s\n", *e);
    return 0;
}
