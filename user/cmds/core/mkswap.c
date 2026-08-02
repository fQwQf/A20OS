#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "../../../kernel/include/mm/swap.h"

#define SYS_mkswap 1020

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-p priority] /dev/vdX\n", prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int priority = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            priority = atoi(argv[++i]);
        } else if (!path) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!path) {
        usage(argv[0]);
        return 1;
    }

    if (syscall(SYS_mkswap, path, priority) < 0) {
        perror("mkswap");
        return 1;
    }

    printf("Setting up swapspace version %u, label = %s, priority = %d\n",
           SWAP_VERSION, path, priority);
    return 0;
}
