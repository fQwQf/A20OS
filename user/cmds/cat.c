/* cat — concatenate files and print on the standard output */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void cat_file(int fd) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(1, buf + written, n - written);
            if (w < 0) return;
            written += w;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        cat_file(0);
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            cat_file(0);
        } else {
            int fd = open(argv[i], O_RDONLY, 0);
            if (fd < 0) {
                printf("cat: %s: No such file or directory\n", argv[i]);
                status = 1;
                continue;
            }
            cat_file(fd);
            close(fd);
        }
    }
    return status;
}
