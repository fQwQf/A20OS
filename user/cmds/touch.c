#include "../lib/libc.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: touch FILE...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            printf("touch: %s: cannot create\n", argv[i]);
            return 1;
        }
        close(fd);
    }
    return 0;
}
