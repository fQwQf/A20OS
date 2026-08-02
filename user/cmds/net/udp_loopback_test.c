#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TEST_PORT 12346
#define TEST_MSG "hello udp loopback"

static int server(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(TEST_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    char buf[64];
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&peer, &peerlen);
    if (n < 0) {
        close(fd);
        return -1;
    }
    buf[n] = '\0';
    int ok = (strcmp(buf, TEST_MSG) == 0);
    if (ok)
        sendto(fd, "ok", 2, 0, (struct sockaddr *)&peer, peerlen);
    close(fd);
    return ok ? 0 : -1;
}

static int client(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(TEST_PORT);

    if (sendto(fd, TEST_MSG, strlen(TEST_MSG), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
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
        printf("UDP_LOOPBACK_TEST: FAIL\n");
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
        printf("UDP_LOOPBACK_TEST: PASS\n");
        return 0;
    }
    printf("UDP_LOOPBACK_TEST: FAIL\n");
    return 1;
}
