#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
    int fd = open("/proc/net", O_RDONLY);
    if (fd < 0) {
        perror("open /proc/net");
        return 1;
    }

    char buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            perror("read /proc/net");
            close(fd);
            return 1;
        }
        if (n == 0)
            break;
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    close(fd);
    return 0;
}
