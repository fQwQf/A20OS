#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>

#define TELNET_PORT 2323
static int write_all(int fd, const void *buffer, size_t length)
{
    const char *p = (const char *)buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static void serve_client(int socket_fd)
{
    static const char banner[] = "\r\nA20OS remote shell\r\n";
    (void)write_all(socket_fd, banner, sizeof(banner) - 1);
    if (dup2(socket_fd, STDIN_FILENO) < 0 ||
        dup2(socket_fd, STDOUT_FILENO) < 0 ||
        dup2(socket_fd, STDERR_FILENO) < 0)
        return;
    if (socket_fd > STDERR_FILENO)
        close(socket_fd);

    char *argv[] = { "mksh", "-i", NULL };
    char *envp[] = {
        "PATH=/bin:/usr/bin", "HOME=/", "SHELL=/bin/mksh",
        "TERM=xterm", "USER=root", NULL
    };
    execve("/bin/mksh", argv, envp);
}

static void reap_children(int signal_number)
{
    (void)signal_number;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : TELNET_PORT;
    if (port <= 0 || port > 65535)
        return 2;

    signal(SIGCHLD, reap_children);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
        return 1;
    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server, 4) < 0) {
        close(server);
        return 1;
    }
    printf("[telnetd] listening on port %d\n", port);

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        pid_t child = fork();
        if (child == 0) {
            close(server);
            serve_client(client);
            close(client);
            _exit(0);
        }
        if (child < 0) {
            close(client);
            continue;
        }
        close(client);
    }
    close(server);
    return 1;
}
