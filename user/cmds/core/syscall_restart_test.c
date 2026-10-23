#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t delivered;

static void handler(int sig) {
    (void)sig;
    delivered++;
}

static int receive_after_signal(int restart) {
    int sockets[2];
    struct sigaction action = {.sa_handler = handler,
                               .sa_flags = restart ? SA_RESTART : 0};
    sigemptyset(&action.sa_mask);
    delivered = 0;
    if (sigaction(SIGUSR1, &action, NULL) ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
        return 1;
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0)
        return 1;
    if (!child) {
        close(sockets[0]);
        usleep(100000);
        if (kill(parent, SIGUSR1))
            _exit(1);
        usleep(100000);
        char payload = 'R';
        struct iovec iov = {&payload, 1};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
        _exit(sendmsg(sockets[1], &msg, 0) != 1);
    }
    close(sockets[1]);
    char payload = 0;
    struct iovec iov = {&payload, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t result = recvmsg(sockets[0], &msg, 0);
    int failed = 0;
    if (!restart) {
        failed = result != -1 || errno != EINTR;
        result = recvmsg(sockets[0], &msg, 0);
    }
    int status = 0;
    failed |= result != 1 || payload != 'R' || delivered != 1;
    failed |= waitpid(child, &status, 0) != child || status != 0;
    close(sockets[0]);
    return failed;
}

int main(void) {
    alarm(10);
    int failed = receive_after_signal(1) | receive_after_signal(0);
    puts(failed ? "SYSCALL_RESTART: FAIL"
                : "SYSCALL_RESTART: PASS recvmsg SA_RESTART + EINTR");
    return failed;
}
