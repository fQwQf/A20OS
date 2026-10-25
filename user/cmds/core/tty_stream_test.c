/* Byte-exact concurrent console writes. The host checks each complete record. */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

int main(void)
{
    pid_t children[4];
    alarm(20);
    for (int writer = 0; writer < 4; writer++) {
        children[writer] = fork();
        if (children[writer] < 0)
            return 1;
        if (!children[writer]) {
            for (int seq = 0; seq < 40; seq++) {
                char record[128];
                int length = snprintf(record, sizeof(record),
                    "TTY_STREAM: writer=%d seq=%02d abcdefghijklmnopqrstuvwxyz0123456789\n",
                    writer, seq);
                if (write(writer & 1 ? 2 : 1, record, (size_t)length) != length)
                    _exit(1);
            }
            _exit(0);
        }
    }
    for (int writer = 0; writer < 4; writer++) {
        int status;
        if (waitpid(children[writer], &status, 0) != children[writer] ||
            !WIFEXITED(status) || WEXITSTATUS(status))
            return 1;
    }
    puts("TTY_STREAM: PASS");
    return 0;
}
