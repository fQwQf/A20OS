#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    printf("  PID TTY          TIME CMD\n");
    syscall(SYS_prctl, 99, 0, 0, 0);
    
    return 0;
}
