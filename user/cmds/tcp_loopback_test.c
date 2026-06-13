#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TEST_PORT 12345
#define TEST_MSG "hello tcp loopback"

static int server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(TEST_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }

    int c = accept(fd, NULL, NULL);
    close(fd);
    if (c < 0)
        return -1;

    char buf[64];
    ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
    if (n < 0) {
        close(c);
        return -1;
    }
    buf[n] = '\0';
    int ok = (strcmp(buf, TEST_MSG) == 0);
    send(c, ok ? "ok" : "no", 2, 0);
    close(c);
    return ok ? 0 : -1;
}

static int client(void) {
    struct timeval tv;
    long start = 0;
    if (gettimeofday(&tv, NULL) == 0)
        start = tv.tv_sec * 1000L + tv.tv_usec / 1000L;

    int fd = -1;
    for (;;) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(TEST_PORT);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            break;
        close(fd);
        fd = -1;
        if (gettimeofday(&tv, NULL) == 0 &&
            (tv.tv_sec * 1000L + tv.tv_usec / 1000L) - start > 2000)
            return -1;
        usleep(10000);
    }

    if (send(fd, TEST_MSG, strlen(TEST_MSG), 0) < 0) {
        close(fd);
        return -1;
    }

    char buf[8];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    close(fd);
    return (n == 2 && memcmp(buf, "ok", 2) == 0) ? 0 : -1;
}

int main(void) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("TCP_LOOPBACK_TEST: FAIL\n");
        return 1;
    }
    if (pid == 0) {
        int r = server();
        _exit(r < 0 ? 1 : 0);
    }

    usleep(100000);
    int r = client();
    int status;
    waitpid(pid, &status, 0);

    if (r == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("TCP_LOOPBACK_TEST: PASS\n");
        return 0;
    }
    printf("TCP_LOOPBACK_TEST: FAIL\n");
    return 1;
}
