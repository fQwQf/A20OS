#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/un.h>

#define TEST_PATH "/tmp/unix_test.sock"
#define TEST_MSG "hello unix"

static int server(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    unlink(TEST_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TEST_PATH, sizeof(addr.sun_path) - 1);

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
    unlink(TEST_PATH);
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
    close(c);
    return ok ? 0 : -1;
}

static int client(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TEST_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (send(fd, TEST_MSG, strlen(TEST_MSG), 0) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int socketpair_test(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;

    if (send(sv[0], "pair", 4, 0) != 4) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    char buf[8];
    ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
    close(sv[0]);
    close(sv[1]);
    return (n == 4 && memcmp(buf, "pair", 4) == 0) ? 0 : -1;
}

int main(void) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("UNIX_TEST: FAIL\n");
        return 1;
    }
    if (pid == 0) {
        int r = server();
        _exit(r < 0 ? 1 : 0);
    }

    usleep(100000);
    int r1 = client();
    int status;
    waitpid(pid, &status, 0);
    int r2 = socketpair_test();

    if (r1 == 0 && r2 == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("UNIX_TEST: PASS\n");
        return 0;
    }
    printf("UNIX_TEST: FAIL\n");
    return 1;
}
