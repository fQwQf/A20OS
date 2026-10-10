/* TCGETS/TCSETS use the kernel wire ABI, not libc's larger struct termios.
 * Put the buffer at a guard page to detect copying past its valid boundary. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    int unlock = 0, number = -1;
    if (master < 0 || ioctl(master, TIOCSPTLCK, &unlock) ||
        ioctl(master, TIOCGPTN, &number))
        return 1;
    char path[64];
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    int slave = open(path, O_RDWR | O_NOCTTY);
    if (slave < 0)
        return 2;
    size_t page = sysconf(_SC_PAGESIZE);
    char *mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED || mprotect(mapping + page, page, PROT_NONE))
        return 3;
    /* The Linux ioctl number embeds the PowerPC termios size.  For the
     * asm-generic TCGETS number the wire layout is four u32 + 20 bytes. */
    size_t wire_size = (TCGETS >> 16) & 0x3fff;
    if (!wire_size)
        wire_size = 36;
    char *wire = mapping + page - wire_size;
    int failed = 0;
    for (unsigned i = 0; i < 2; i++) {
        int fd = i ? slave : master;
        if (syscall(SYS_ioctl, fd, TCGETS, wire) < 0) {
            failed = 1;
            break;
        }
        uint32_t original;
        memcpy(&original, wire + 12, sizeof(original));
        uint32_t changed = original ^ 8; /* ECHO */
        memcpy(wire + 12, &changed, sizeof(changed));
        if (syscall(SYS_ioctl, fd, TCSETS, wire) < 0 ||
            syscall(SYS_ioctl, fd, TCGETS, wire) < 0 ||
            memcmp(wire + 12, &changed, sizeof(changed))) {
            failed = 1;
            break;
        }
        memcpy(wire + 12, &original, sizeof(original));
        if (syscall(SYS_ioctl, fd, TCSETS, wire) < 0)
            failed = 1;
    }
    munmap(mapping, page * 2);
    close(slave);
    close(master);
    printf("PTY_TERMIOS: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
