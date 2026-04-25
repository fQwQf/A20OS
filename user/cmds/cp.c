/* cp — copy files */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("cp: missing file operand\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    int fd_in = open(src, O_RDONLY, 0);
    if (fd_in < 0) {
        printf("cp: cannot stat '%s': No such file or directory\n", src);
        return 1;
    }

    struct stat st;
    if (fstat(fd_in, &st) < 0) {
        close(fd_in);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        printf("cp: -r not specified; omitting directory '%s'\n", src);
        close(fd_in);
        return 1;
    }

    /* Check if dst is a directory */
    struct stat dst_st;
    char target[512];
    if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        snprintf(target, sizeof(target), "%s/%s", dst, base);
        dst = target;
    }

    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (fd_out < 0) {
        printf("cp: cannot create regular file '%s'\n", dst);
        close(fd_in);
        return 1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd_in, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(fd_out, buf + written, n - written);
            if (w < 0) {
                printf("cp: write error\n");
                close(fd_in);
                close(fd_out);
                return 1;
            }
            written += w;
        }
    }

    fsync(fd_out);

    close(fd_in);
    close(fd_out);
    return 0;
}
