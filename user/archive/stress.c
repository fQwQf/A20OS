#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("Starting fork/exec stress test\n");
    for (int i = 0; i < 50; i++) {
        int pid = fork();
        if (pid < 0) {
            printf("fork failed at iteration %d\n", i);
            break;
        }
        if (pid == 0) {
            char *args[] = {"/bin/sh", "-c", "echo 1", NULL};
            execve("/bin/sh", args, NULL);
            _exit(1);
        }
    }
    for (int i = 0; i < 50; i++) {
        wait(NULL);
    }
    printf("Stress test done\n");
    return 0;
}
