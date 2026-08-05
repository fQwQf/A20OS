/*
 * netd socket proxy e2e.  Run as:
 *   netd_sock_test server   -> TCP echo server on :9999 (through netd proxy)
 *   netd_sock_test          -> TCP client echoing to 10.0.2.2:9999
 * The host can reach the guest server via hostfwd (127.0.0.1:18080).
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static void die(const char *m) { perror(m); exit(1); }

static void run_server(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    if (l < 0) die("server socket");
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(9999);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(l, (struct sockaddr *)&a, sizeof(a)) < 0) die("bind");
    if (listen(l, 4) < 0) die("listen");
    printf("NETD_SOCK_SERVER: listening\n");
    fflush(stdout);
    for (;;) {
        int c = accept(l, NULL, NULL);
        if (c < 0) die("accept");
        char buf[64];
        int n = read(c, buf, sizeof(buf));
        if (n < 0) die("server read");
        write(c, buf, n);
        close(c);
    }
}

static void run_client(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("client socket");
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(9999);
    a.sin_addr.s_addr = inet_addr("10.0.2.2");
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0)
        die("connect");
    const char *msg = "netd-sock-echo";
    if (write(s, msg, strlen(msg)) != (ssize_t)strlen(msg))
        die("write");
    char buf[64];
    int n = read(s, buf, sizeof(buf));
    if (n != (int)strlen(msg) || memcmp(buf, msg, n) != 0) {
        fprintf(stderr, "echo mismatch n=%d\n", n);
        return;
    }
    close(s);
    printf("NETD_SOCK_TEST: PASS\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "server") == 0)
        run_server();
    else
        run_client();
    return 0;
}
