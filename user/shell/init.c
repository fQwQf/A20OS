/*
 * A20OS init program
 */

#include "../lib/libc.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("[INIT] Starting shell...\n");

    char *sh_argv[] = {"sh", NULL};
    execve("/mnt/sh", sh_argv, NULL);

    printf("[INIT] execve failed!\n");
    while (1) {}
    _exit(0);
}
