#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("Rebooting system.\n");
    syscall(SYS_reboot, 0x424F4F54);
    printf("reboot: failed\n");
    return 1;
}
