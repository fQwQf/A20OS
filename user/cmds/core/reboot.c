#include <stdio.h>
#include <sys/reboot.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("Rebooting system.\n");
    fflush(stdout);
    if (reboot(RB_AUTOBOOT) == 0)
        return 0;
    printf("reboot: failed\n");
    return 1;
}
