#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>

#define SOCKET_STRESS_PORT  41000
#define ITERATIONS          50
#define PAYLOAD             "a20os-socket-stress-payload"
#define PAYLOAD_LEN         (sizeof(PAYLOAD) - 1)

static int fail(const char *what)
{
    printf("SOCKET_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static uint16_t bswap16(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

static int tcp_server_loop(int ready_fd)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return fail("tcp-server-socket");

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = 0x0100007fU;
    addr.sin_port = bswap16(SOCKET_STRESS_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return fail("tcp-server-bind");
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return fail("tcp-server-listen");
    }
    if (ready_fd >= 0) {
        char ready = 'R';
        if (write(ready_fd, &ready, 1) != 1) {
            close(ready_fd);
            close(fd);
            return fail("tcp-server-ready");
        }
        close(ready_fd);
    }

    for (int i = 0; i < ITERATIONS; i++) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            close(fd);
            return fail("tcp-server-accept");
        }
        char buf[64];
        ssize_t n = recv(cfd, buf, sizeof(buf), 0);
        if (n != PAYLOAD_LEN || memcmp(buf, PAYLOAD, PAYLOAD_LEN) != 0) {
            close(cfd);
            close(fd);
            return fail("tcp-server-recv");
        }
        if (send(cfd, PAYLOAD, PAYLOAD_LEN, 0) != PAYLOAD_LEN) {
            close(cfd);
            close(fd);
            return fail("tcp-server-send");
        }
        close(cfd);
    }

    close(fd);
    return 0;
}

static int tcp_client_loop(void)
{
    for (int i = 0; i < ITERATIONS; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return fail("tcp-client-socket");

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = 0x0100007fU;
        addr.sin_port = bswap16(SOCKET_STRESS_PORT);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return fail("tcp-client-connect");
        }
        if (send(fd, PAYLOAD, PAYLOAD_LEN, 0) != PAYLOAD_LEN) {
            close(fd);
            return fail("tcp-client-send");
        }
        char buf[64];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n != PAYLOAD_LEN || memcmp(buf, PAYLOAD, PAYLOAD_LEN) != 0) {
            close(fd);
            return fail("tcp-client-recv");
        }
        close(fd);
    }
    return 0;
}

static int udp_ping_pong(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return fail("udp-fork");

    if (pid == 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            _exit(1);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = 0x0100007fU;
        addr.sin_port = bswap16(SOCKET_STRESS_PORT + 1);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            _exit(2);
        }

        for (int i = 0; i < ITERATIONS; i++) {
            char buf[64];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n != PAYLOAD_LEN || memcmp(buf, PAYLOAD, PAYLOAD_LEN) != 0) {
                close(fd);
                _exit(3);
            }
            if (sendto(fd, PAYLOAD, PAYLOAD_LEN, 0,
                       (struct sockaddr *)&from, fromlen) != PAYLOAD_LEN) {
                close(fd);
                _exit(4);
            }
        }
        close(fd);
        _exit(0);
    }

    usleep(200000);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return fail("udp-client-socket");

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = 0x0100007fU;
    dst.sin_port = bswap16(SOCKET_STRESS_PORT + 1);

    for (int i = 0; i < ITERATIONS; i++) {
        if (sendto(fd, PAYLOAD, PAYLOAD_LEN, 0,
                   (struct sockaddr *)&dst, sizeof(dst)) != PAYLOAD_LEN) {
            close(fd);
            return fail("udp-client-sendto");
        }
        char buf[64];
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n != PAYLOAD_LEN || memcmp(buf, PAYLOAD, PAYLOAD_LEN) != 0) {
            close(fd);
            return fail("udp-client-recvfrom");
        }
    }
    close(fd);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return fail("udp-wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("udp-server-fail");
    return 0;
}

static int concurrent_tcp_stress(void)
{
    int ready_pipe[2];
    if (pipe(ready_pipe) < 0)
        return fail("tcp-ready-pipe");

    pid_t pid = fork();
    if (pid < 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return fail("tcp-fork");
    }

    if (pid == 0) {
        close(ready_pipe[0]);
        int r = tcp_server_loop(ready_pipe[1]);
        _exit(r);
    }

    close(ready_pipe[1]);
    char ready = 0;
    ssize_t ready_n = read(ready_pipe[0], &ready, 1);
    close(ready_pipe[0]);
    if (ready_n != 1 || ready != 'R') {
        int status = 0;
        (void)waitpid(pid, &status, 0);
        return fail("tcp-server-ready");
    }

    int r = tcp_client_loop();

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return fail("tcp-wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("tcp-server-fail");
    return r;
}

int main(void)
{
    printf("SOCKET_STRESS: start\n");
    if (concurrent_tcp_stress() != 0)
        return 1;
    if (udp_ping_pong() != 0)
        return 1;
    printf("SOCKET_STRESS: PASS\n");
    return 0;
}
