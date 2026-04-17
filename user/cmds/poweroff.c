#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("System is going down for power-off NOW.\n");
    syscall(SYS_reboot, 0);
    printf("poweroff: failed\n");
    return 1;
}
