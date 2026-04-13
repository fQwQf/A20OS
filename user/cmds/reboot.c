#include "../lib/libc.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("Rebooting system.\n");
    syscall1(SYS_reboot, 0x424F4F54);
    printf("reboot: failed\n");
    return 1;
}
