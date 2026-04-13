#include "../lib/libc.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("System is going down for power-off NOW.\n");
    syscall1(SYS_reboot, 0);
    printf("poweroff: failed\n");
    return 1;
}
