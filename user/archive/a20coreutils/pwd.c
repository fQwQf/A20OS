#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    char buf[256];
    if (getcwd(buf, sizeof(buf)))
        printf("%s\n", buf);
    else
        printf("pwd: cannot get current directory\n");
    return 0;
}
