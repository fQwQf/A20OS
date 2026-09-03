/* a20mount: minimal mounter for A20OS block devices.
 *
 * busybox mount(8) stats the device node first, but A20OS addresses
 * block devices synthetically (/dev/vdN parsed by the VFS, no devfs
 * node), so the applet can never reach the syscall.  This helper calls
 * mount(2) directly.
 *
 * Usage: a20mount <dev> <mountpoint> <fstype>
 */
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: a20mount <dev> <mountpoint> <fstype>\n");
        return 2;
    }
    mkdir(argv[2], 0755);
    if (mount(argv[1], argv[2], argv[3], 0, NULL) < 0) {
        perror("mount");
        return 1;
    }
    return 0;
}
