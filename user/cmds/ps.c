#include "../lib/libc.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    printf("  PID TTY          TIME CMD\n");
    syscall4(SYS_prctl, 99, 0, 0, 0);
    
    return 0;
}
